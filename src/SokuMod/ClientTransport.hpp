#ifndef SOKULOBBIES_CLIENT_TRANSPORT_HPP
#define SOKULOBBIES_CLIENT_TRANSPORT_HPP

#include <Packet.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>
#include <exception>
#include <optional>
#include <unordered_map>
#include <utility>
#include <cstdio>

// Publish cancellation under the same lock used by the waiter, so a notify
// cannot fall between checking the flag and entering wait().
inline void stopClientWaiter(std::atomic_bool &running, std::mutex &mutex, std::condition_variable &wake) {
	{ std::lock_guard<std::mutex> lock(mutex); running = false; }
	wake.notify_all();
}

template<class Cleanup>
class ClientConstructionGuard {
	Cleanup _cleanup;
	bool _active = true;
public:
	explicit ClientConstructionGuard(Cleanup cleanup) : _cleanup(std::move(cleanup)) {}
	ClientConstructionGuard(const ClientConstructionGuard &) = delete;
	~ClientConstructionGuard() noexcept { if (_active) _cleanup(); }
	void dismiss() noexcept { _active = false; }
};

class ClientArcadeMembership {
	std::unordered_map<uint32_t, uint32_t> _machines;
public:
	std::optional<uint32_t> machineOf(uint32_t player) const {
		auto found = _machines.find(player);
		return found == _machines.end() ? std::nullopt : std::optional<uint32_t>{found->second};
	}
	bool enter(uint32_t player, uint32_t machine) {
		if (machineOf(player) == machine) return false;
		_machines.insert_or_assign(player, machine); return true;
	}
	bool leave(uint32_t player, uint32_t machine) {
		if (machineOf(player) != machine) return false;
		_machines.erase(player); return true;
	}
	unsigned count(uint32_t machine) const {
		unsigned result = 0;
		for (const auto &entry : _machines) if (entry.second == machine) ++result;
		return result;
	}
};

inline bool validClientIndex(size_t index, size_t count) { return index < count; }

// No C++ exception may escape a std::thread entry point. Reporting can itself
// fail (notably on allocation failure), so cancellation must be independent.
template<class Work, class Report, class Stop>
void runClientWorker(Work work, Report report, Stop stop) noexcept {
	try { work(); return; }
	catch (const std::exception &error) {
		try { report(error.what()); } catch (...) {}
	}
	catch (...) {
		try { report("Unknown client worker exception"); } catch (...) {}
	}
	try { stop(); } catch (...) {}
}

// A recoverable data/request failure must not tear down a long-lived worker.
template<class Work, class Report>
bool runClientTask(Work work, Report report) noexcept {
	try { work(); return true; }
	catch (const std::exception &error) { try { report(error.what()); } catch (...) {} }
	catch (...) { try { report("Unknown client task exception"); } catch (...) {} }
	return false;
}

template<class Work>
void runClientCleanup(const char *label, Work work) noexcept {
	runClientWorker(work, [label](const char *reason) {
		fprintf(stderr, "Client %s failed: %s\n", label, reason);
	}, [] {});
}

template<class Report, class Disconnect>
void runClientDisconnectCallbacks(Report report, Disconnect disconnect) noexcept {
	runClientCleanup("connection error callback", report);
	runClientCleanup("connection disconnect callback", disconnect);
}

// Return an empty optional on failure, allowing the native window procedure to
// run once, outside our exception guard and after all local locks are released.
template<class Work>
auto runClientWindowCallback(const char *label, Work work) noexcept -> decltype(work()) {
	try { return work(); }
	catch (const std::exception &error) { fprintf(stderr, "Client %s failed: %s\n", label, error.what()); }
	catch (...) { fprintf(stderr, "Client %s failed: unknown exception\n", label); }
	return {};
}

inline bool validClientBattleStatus(Lobbies::BattleStatus status) {
	return status >= Lobbies::BATTLE_STATUS_IDLE && status <= Lobbies::BATTLE_STATUS_SPECTATING;
}

inline bool validClientGameInfoResult(uint64_t requestedSession, uint64_t currentSession,
	uint8_t requestedMachine, uint8_t currentMachine, Lobbies::BattleStatus status) {
	return requestedSession == currentSession && requestedMachine == currentMachine &&
		(status == Lobbies::BATTLE_STATUS_WAITING || status == Lobbies::BATTLE_STATUS_PLAYING);
}

// Copies complete wire frames; no UI pointers or references cross the thread boundary.
class ClientInbox {
public:
	struct Frame { std::vector<char> bytes; uint64_t session; bool chat; };
	static constexpr size_t MAX_FRAMES = 1024, MAX_BYTES = 4 * 1024 * 1024, MAX_CHAT = 256;
	bool push(const void *data, size_t size, uint64_t session, bool chat, const std::atomic_bool &running) {
		if (size > MAX_BYTES) return false;
		std::unique_lock<std::mutex> lock(_mutex);
		if (!running) return false;
		if (chat && (_chat >= MAX_CHAT || _frames.size() >= MAX_FRAMES - 128)) { ++_dropped; return true; }
		_space.wait(lock, [&] { return !running || (_frames.size() < MAX_FRAMES && _bytes + size <= MAX_BYTES); });
		if (!running) return false;
		_frames.push_back({std::vector<char>(static_cast<const char *>(data), static_cast<const char *>(data) + size), session, chat});
		_bytes += size; _chat += chat;
		return true;
	}
	bool pop(Frame &frame) {
		std::lock_guard<std::mutex> lock(_mutex);
		if (_frames.empty()) return false;
		frame = std::move(_frames.front()); _frames.pop_front();
		_bytes -= frame.bytes.size(); _chat -= frame.chat;
		_space.notify_one(); return true;
	}
	void clear() {
		std::lock_guard<std::mutex> lock(_mutex);
		_frames.clear(); _bytes = _chat = 0; _space.notify_all();
	}
	bool empty() const { std::lock_guard<std::mutex> lock(_mutex); return _frames.empty(); }
	std::pair<size_t, size_t> sizeSnapshot() const { std::lock_guard<std::mutex> lock(_mutex); return {_frames.size(), _bytes}; }
	void wake() { std::lock_guard<std::mutex> lock(_mutex); _space.notify_all(); }
	uint64_t takeDropped() { std::lock_guard<std::mutex> lock(_mutex); auto count = _dropped; _dropped = 0; return count; }
private:
	mutable std::mutex _mutex;
	std::condition_variable _space;
	std::deque<Frame> _frames;
	size_t _bytes = 0, _chat = 0;
	uint64_t _dropped = 0;
};

inline size_t clientPacketSize(uint8_t opcode) {
	switch (opcode) {
#define CLIENT_FRAME(op, type) case Lobbies::op: return sizeof(Lobbies::type)
	CLIENT_FRAME(OPCODE_OLLEH, PacketOlleh);
	CLIENT_FRAME(OPCODE_PLAYER_JOIN, PacketPlayerJoin);
	CLIENT_FRAME(OPCODE_PLAYER_LEAVE, PacketPlayerLeave);
	CLIENT_FRAME(OPCODE_KICKED, PacketKicked);
	CLIENT_FRAME(OPCODE_MOVE, PacketMove);
	CLIENT_FRAME(OPCODE_POSITION, PacketPosition);
	CLIENT_FRAME(OPCODE_GAME_REQUEST, PacketGameRequest);
	CLIENT_FRAME(OPCODE_GAME_START, PacketGameStart);
	CLIENT_FRAME(OPCODE_PONG, PacketPong);
	CLIENT_FRAME(OPCODE_SETTINGS_UPDATE, PacketSettingsUpdate);
	CLIENT_FRAME(OPCODE_ARCADE_ENGAGE, PacketArcadeEngage);
	CLIENT_FRAME(OPCODE_ARCADE_LEAVE, PacketArcadeLeave);
	CLIENT_FRAME(OPCODE_MESSAGE, PacketMessage);
	CLIENT_FRAME(OPCODE_IMPORTANT_MESSAGE, PacketImportantMessage);
	CLIENT_FRAME(OPCODE_BATTLE_STATUS_UPDATE, PacketBattleStatusUpdate);
#undef CLIENT_FRAME
	default: return 0;
	}
}
#endif
