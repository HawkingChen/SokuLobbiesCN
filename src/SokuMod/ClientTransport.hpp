#ifndef SOKULOBBIES_CLIENT_TRANSPORT_HPP
#define SOKULOBBIES_CLIENT_TRANSPORT_HPP

#include <Packet.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

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
