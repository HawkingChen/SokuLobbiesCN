//
// Created by PinkySmile on 01/10/2022.
//

#ifndef _LOBBYNOLOG
#include <iostream>
#include <mutex>
std::mutex logMutex;
#endif
#include <cstring>
#include <array>
#include <algorithm>
#include <chrono>
#include <vector>
#include <functional>
#include <Exceptions.hpp>
#include "Connection.hpp"
#include "getPublicIp.hpp"
#include "ClientPerformance.hpp"
#include "ipv6map_extern.hpp"

#define print(...) do { if (this->_init) printf(__VA_ARGS__); } while(0)

extern unsigned char soku2Major;
extern unsigned char soku2Minor;
extern char soku2Letter;
extern bool soku2Force;
extern bool chineseLanguage;

static long long steadyClockMilliseconds()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()
	).count();
}

namespace {
	std::mutex clientRegistryMutex;
	std::vector<std::weak_ptr<Connection>> clientRegistry;
}

void Connection::_netLoop()
{
	std::array<char, sizeof(Lobbies::Packet) * 6> buffer{};
	size_t buffered = 0;
	while (_connected) {
		if (!_hasConnected) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
		const auto fd = _socket.getSockFd();
		fd_set reads; FD_ZERO(&reads); FD_SET(fd, &reads);
		timeval timeout{0, 50000};
		const auto ready = select(static_cast<int>(fd + 1), &reads, nullptr, nullptr, &timeout);
		if (!_connected) break;
		if (!ready) continue;
		if (ready < 0) { _failTransport(getLastSocketError()); break; }
		const auto received = ::recv(fd, buffer.data() + buffered, static_cast<int>(buffer.size() - buffered), 0);
		if (received < 0) {
			if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
			_failTransport(getLastSocketError()); break;
		}
		if (!received) { _failTransport("Connection closed"); break; }
		buffered += received;
		size_t consumed = 0;
		while (consumed < buffered && _connected) {
			const auto opcode = static_cast<uint8_t>(buffer[consumed]);
			const auto length = clientPacketSize(opcode);
			if (!length) { _failTransport("Protocol error: Invalid opcode " + std::to_string(opcode)); return; }
			if (buffered - consumed < length) break;
			Lobbies::Packet packet{};
			std::memcpy(&packet, buffer.data() + consumed, length);
			consumed += length;
			if (opcode == Lobbies::OPCODE_PONG) {
				// Lobby-list metadata has no UI callbacks and must remain live while hidden.
				auto size = length; _handlePacket(packet.pong, size); continue;
			}
			uint64_t session;
			{
				std::lock_guard<std::mutex> lock(_sessionMutex);
				if (opcode == Lobbies::OPCODE_OLLEH) {
					if (_helloSessions.empty()) { _failTransport("Protocol error: Unexpected handshake"); return; }
					_receiveSession = _helloSessions.front(); _helloSessions.pop_front();
				}
				session = _receiveSession;
			}
			// Authentication can reject HELLO with KICKED before any OLLEH arrives.
			// It terminates the transport, so retain its reason across lobby epochs.
			if (opcode == Lobbies::OPCODE_KICKED) session = _session.load();
			if (!session || session != _session.load()) continue;
			// Never shed server errors, private messages or blacklist control replies.
			const bool chat = opcode == Lobbies::OPCODE_MESSAGE && packet.message.playerId != 0 && packet.message.channelId >= 0;
			if (!_inbox.push(&packet, length, session, chat, _connected)) return;
		}
		if (consumed) { buffered -= consumed; std::memmove(buffer.data(), buffer.data() + consumed, buffered); }
	}
}

Connection::Connection(const std::string &host, unsigned short port, const Player &initParams) :
	_host(host),
	_port(port),
	_initParams(initParams)
{
}

Connection::~Connection()
{
	_ipLookupAlive = false;
	this->_init = false;
	_stopTransport();
	ClientConstructionGuard cleanup([this]() noexcept {
		runClientCleanup("IP lookup join", [&] { if (_ipLookup.valid()) _ipLookup.wait(); });
		runClientCleanup("connect thread join", [&] { if (_connectThread.joinable()) _connectThread.join(); });
		runClientCleanup("receive thread join", [&] { if (_netThread.joinable()) _netThread.join(); });
		runClientCleanup("send thread join", [&] { if (_sendThread.joinable()) _sendThread.join(); });
		runClientCleanup("connection socket close", [&] { _socket.disconnect(); });
	});
	runClientCleanup("disconnect callback", [this] {
		std::lock_guard<std::mutex> meMutexGuard(this->meMutex);
		std::lock_guard<std::mutex> playerMutexGuard(this->_playerMutex);
		std::lock_guard<std::mutex> functionMutexGuard(this->functionMutex);
		if (!this->_disconnectNotified && this->onDisconnect) {
			_disconnectNotified = true;
			this->onDisconnect();
		}
	});
}

void Connection::error(const std::string &msg)
{
	_failTransport(msg);
	_inbox.clear();
}

void Connection::_failTransport(const std::string &reason)
{
	{ std::lock_guard<std::mutex> lock(_errorMutex); if (_transportError.empty()) _transportError = reason; }
	_stopTransport();
}

void Connection::_stopTransport()
{
	stopClientWaiter(_connected, _sendMutex, _sendReady);
	_inbox.wake();
}

void Connection::startThread()
{
	ClientConstructionGuard rollback([this]() noexcept {
		_stopTransport();
		if (_connectThread.joinable()) _connectThread.join();
		if (_netThread.joinable()) _netThread.join();
		if (_sendThread.joinable()) _sendThread.join();
		_socket.disconnect();
	});
	{ std::lock_guard<std::mutex> lock(clientRegistryMutex); clientRegistry.emplace_back(shared_from_this()); }
	this->_connectThread = std::thread(&Connection::_runWorker, this, &Connection::_connectLoop, "connect");
	this->_netThread = std::thread(&Connection::_runWorker, this, &Connection::_netLoop, "receive");
	_sendThread = std::thread(&Connection::_runWorker, this, &Connection::_sendLoop, "send");
	rollback.dismiss();
}

void Connection::_connectLoop()
{
	_socket.connectWithTimeout(_host, _port, std::chrono::seconds(6), _connected);
	{ std::lock_guard<std::mutex> lock(_sendMutex); if (_connected) _hasConnected = true; }
	_sendReady.notify_all();
}

void Connection::_runWorker(void (Connection::*work)(), const char *label) noexcept
{
	runClientWorker([&] { (this->*work)(); }, [&](const char *reason) {
		fprintf(stderr, "Lobby %s worker failed: %s\n", label, reason);
		_failTransport(std::string("Lobby ") + label + " worker failed: " + reason);
	}, [this] {
		// Allocation-free fallback, even if constructing the error text failed.
		_stopTransport();
	});
}

void Connection::send(const void *packet, size_t size)
{
	if (size == sizeof(Lobbies::PacketMessage) && *static_cast<const uint8_t *>(packet) == Lobbies::OPCODE_MESSAGE) {
		const auto &message = *static_cast<const Lobbies::PacketMessage *>(packet);
		const std::string_view text(message.message, strnlen(message.message, sizeof(message.message)));
		// Internal synchronization must never keep an otherwise idle player alive.
		if (_init && !IdleTracking::isControl(text) && text.substr(0, 8) != "/__block") {
			std::lock_guard<std::mutex> lock(_idleMutex);
			_idleClock.active(IdleTracking::Clock::now());
		}
	}
	if (size && *static_cast<const uint8_t *>(packet) == Lobbies::OPCODE_ARCADE_LEAVE)
		_gameInfoPending = false;
	if (!size || !_connected) return;
	bool overflow = false;
	{
		std::lock_guard<std::mutex> lock(_sendMutex);
		if (*static_cast<const uint8_t *>(packet) == Lobbies::OPCODE_POSITION && !_init) return;
		if (*static_cast<const uint8_t *>(packet) == Lobbies::OPCODE_BATTLE_STATUS_UPDATE && size == sizeof(Lobbies::PacketBattleStatusUpdate)) {
			std::lock_guard<std::mutex> snapshotLock(_heartbeatMutex);
			_heartbeat.status = static_cast<const Lobbies::PacketBattleStatusUpdate *>(packet)->newStatus;
		}
		if (_outgoing.size() >= 1024 || size > 512 * 1024 || _outgoingBytes > 512 * 1024 - size) overflow = true;
		else {
			const auto bytes = static_cast<const char *>(packet);
			_outgoing.emplace_back(std::make_shared<std::vector<char>>(bytes, bytes + size)); _outgoingBytes += size;
		}
	}
	if (overflow) _failTransport("Lobby send queue limit exceeded");
	else _sendReady.notify_one();
}

void Connection::_sendLoop()
{
	auto nextHeartbeat = std::chrono::steady_clock::now();
	while (_connected) {
		if (_init && _heartbeatReady && std::chrono::steady_clock::now() >= nextHeartbeat) {
			_queueHeartbeat(); nextHeartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		}
		std::shared_ptr<std::vector<char>> frame;
		{
			std::unique_lock<std::mutex> lock(_sendMutex);
			if (!_connected) break;
			if (!_hasConnected || _outgoing.empty()) {
				if (_init && _heartbeatReady) _sendReady.wait_until(lock, nextHeartbeat);
				else _sendReady.wait(lock);
				continue;
			}
			frame = _outgoing.front();
		}
		size_t offset = 0;
		auto progress = std::chrono::steady_clock::now();
		while (_connected && offset < frame->size()) {
			const auto fd = _socket.getSockFd();
			fd_set writes; FD_ZERO(&writes); FD_SET(fd, &writes); timeval timeout{0, 50000};
			const auto ready = select(static_cast<int>(fd + 1), nullptr, &writes, nullptr, &timeout);
			if (!_connected) break;
			if (ready < 0) { _failTransport(getLastSocketError()); break; }
			if (ready) {
				const auto sent = ::send(fd, frame->data() + offset, static_cast<int>(frame->size() - offset), 0);
				if (sent > 0) { offset += sent; progress = std::chrono::steady_clock::now(); }
				else if (!sent || WSAGetLastError() != WSAEWOULDBLOCK) { _failTransport(getLastSocketError()); break; }
			}
			if (std::chrono::steady_clock::now() - progress >= std::chrono::seconds(60)) { _failTransport("Lobby send stalled"); break; }
		}
		if (offset == frame->size()) {
			std::lock_guard<std::mutex> lock(_sendMutex);
			_outgoingBytes -= frame->size(); _outgoing.pop_front();
		}
	}
}

void Connection::_publishHeartbeat()
{
	bool published = false;
	{
		std::lock_guard<std::mutex> lock(meMutex);
		if (_init && _me) {
			std::lock_guard<std::mutex> snapshotLock(_heartbeatMutex);
			_heartbeat = Lobbies::PacketPosition{0, _me->pos.x, _me->pos.y, _me->dir, _me->battleStatus};
			published = true;
		}
	}
	if (published) {
		std::lock_guard<std::mutex> lock(_sendMutex);
		if (_init && !_heartbeatReady.exchange(true)) _sendReady.notify_one();
	}
}

void Connection::_queueHeartbeat()
{
	bool overflow = false;
	{
		// Status updates and LEAVE share this ordering lock with heartbeats.
		std::lock_guard<std::mutex> lock(_sendMutex);
		if (!_connected || !_init || !_heartbeatReady) return;
		std::lock_guard<std::mutex> snapshotLock(_heartbeatMutex);
		if (_outgoing.size() >= 1024 || _outgoingBytes > 512 * 1024 - sizeof(_heartbeat)) overflow = true;
		else {
			const auto bytes = reinterpret_cast<const char *>(&_heartbeat);
			_outgoing.emplace_back(std::make_shared<std::vector<char>>(bytes, bytes + sizeof(_heartbeat)));
			_outgoingBytes += sizeof(_heartbeat);
		}
	}
	if (overflow) _failTransport("Lobby send queue limit exceeded");
	else _sendReady.notify_one();
}

void Connection::_pumpEvents(std::chrono::steady_clock::time_point deadline)
{
	if (_helloRequested.exchange(false) && _connected) {
		_disconnectNotified = false; _receivedKick = false;
		try { _sendHello(); } catch (const std::exception &e) { error(e.what()); }
	}
	ClientInbox::Frame frame;
	for (unsigned i = 0; i < 16 && std::chrono::steady_clock::now() < deadline && _inbox.pop(frame); ++i) {
		if (frame.session != _session.load()) continue;
		Lobbies::Packet packet{}; std::memcpy(&packet, frame.bytes.data(), frame.bytes.size());
		auto size = frame.bytes.size();
		try { _handlePacket(packet, size); }
		catch (const std::exception &e) { _failTransport(e.what()); _inbox.clear(); break; }
		if (_receivedKick) { _inbox.clear(); break; }
	}
	_finishGameInfo();
	if (!_connected && _inbox.empty() && !_disconnectNotified) {
		_disconnectNotified = true; _init = false; _helloPending = false;
		std::lock_guard<std::mutex> lock(functionMutex);
		runClientDisconnectCallbacks([&] {
			std::string reason;
			{ std::lock_guard<std::mutex> errorLock(_errorMutex); reason = _transportError; }
			if (!_receivedKick && !reason.empty() && onError) onError(reason);
		}, [&] { if (onDisconnect) onDisconnect(); });
	}
	_skippedChat += _inbox.takeDropped();
	if (_init && _skippedChat && std::chrono::steady_clock::now() - _lastChatNotice >= std::chrono::seconds(5)) {
		std::lock_guard<std::mutex> lock(functionMutex);
		if (onMsg) onMsg(0xAFAFAF, 0, (chineseLanguage ? "聊天消息过多，已略过部分消息：" : "Excess chat messages skipped: ") + std::to_string(_skippedChat), "");
		_skippedChat = 0; _lastChatNotice = std::chrono::steady_clock::now();
	}
	_publishHeartbeat();
	_updateIdleReport();
}

void Connection::_updateIdleReport()
{
	if (!_init || !_me) return;
	const auto now = IdleTracking::Clock::now();
	const auto playingScene = [](SokuLib::Scene scene) {
		return scene == SokuLib::SCENE_SELECTCL || scene == SokuLib::SCENE_SELECTSV ||
			scene == SokuLib::SCENE_LOADINGCL || scene == SokuLib::SCENE_LOADINGSV ||
			scene == SokuLib::SCENE_BATTLECL || scene == SokuLib::SCENE_BATTLESV;
	};
	const bool spectating = _spectatingScene || _spectatingArcade || _me->battleStatus == Lobbies::BATTLE_STATUS_SPECTATING;
	const bool exempt = !spectating && (_me->battleStatus == Lobbies::BATTLE_STATUS_WAITING ||
		_me->battleStatus == Lobbies::BATTLE_STATUS_PLAYING || playingScene(SokuLib::sceneId) || playingScene(SokuLib::newSceneId));
	std::string command;
	{
		std::lock_guard<std::mutex> lock(_idleMutex);
		_idleClock.setExempt(exempt, now);
		if (!_idleClock.due(now)) return;
		command = "/__idle " + std::to_string(_idleClock.idle(now).count()) + (exempt ? " 1" : " 0");
		_idleClock.lastReport = now;
		_idleClock.dirty = false;
	}
	Lobbies::PacketMessage report{0, 0, command};
	send(&report, sizeof(report));
}

void Connection::pumpNetworkEvents()
{
	ClientPerformance::Scope performance(ClientPerformance::Stage::NetworkPump);
	std::vector<std::shared_ptr<Connection>> connections;
	{
		std::lock_guard<std::mutex> lock(clientRegistryMutex);
		for (auto it = clientRegistry.begin(); it != clientRegistry.end();) {
			if (auto connection = it->lock()) { connections.push_back(std::move(connection)); ++it; }
			else it = clientRegistry.erase(it);
		}
	}
	static size_t cursor = 0;
#ifdef SOKU_CLIENT_PERFORMANCE
	static uint64_t lastSnapshot = 0;
	if (ClientPerformance::snapshotDue(lastSnapshot)) {
		size_t frames = 0, bytes = 0, outgoing = 0;
		for (const auto &connection : connections) {
			const auto inbox = connection->_inbox.sizeSnapshot(); frames += inbox.first; bytes += inbox.second;
			std::lock_guard<std::mutex> lock(connection->_sendMutex); outgoing += connection->_outgoingBytes;
		}
		ClientPerformance::gauge(ClientPerformance::Gauge::Connections, connections.size());
		ClientPerformance::gauge(ClientPerformance::Gauge::InboxFrames, frames);
		ClientPerformance::gauge(ClientPerformance::Gauge::InboxBytes, bytes);
		ClientPerformance::gauge(ClientPerformance::Gauge::SendBytes, outgoing);
	}
#endif
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
	for (size_t i = 0; i < connections.size() && std::chrono::steady_clock::now() < deadline; ++i) {
		auto &connection = connections[(cursor + i) % connections.size()];
		try { connection->_pumpEvents(deadline); }
		catch (const std::exception &e) { connection->error(e.what()); }
		catch (...) { connection->error("Client event handler failed"); }
	}
	if (!connections.empty()) cursor = (cursor + 1) % connections.size();
}

bool Connection::isInit() const
{
	return this->_init;
}

bool Connection::hasConnected() const
{
	return this->_hasConnected;
}

bool Connection::isConnected() const
{
	return this->_connected;
}

bool Connection::_handlePacket(const Lobbies::Packet &packet, size_t &size)
{
#ifndef _LOBBYNOLOG
	logMutex.lock();
	std::cout << "[<" << inet_ntoa(this->_socket.getRemote().sin_addr) << ":" << this->_socket.getRemote().sin_port;
	std::cout << "] " << size << " bytes: " << packet.toString() << std::endl;
	logMutex.unlock();
#endif
	std::lock_guard<std::mutex> meMutexGuard(this->meMutex);
	std::lock_guard<std::mutex> playerMutexGuard(this->_playerMutex);
	std::lock_guard<std::mutex> functionMutexGuard(this->functionMutex);
	switch (packet.opcode) {
	case Lobbies::OPCODE_HELLO:
		return this->_handlePacket(packet.hello, size);
	case Lobbies::OPCODE_OLLEH:
		return this->_handlePacket(packet.olleh, size);
	case Lobbies::OPCODE_PLAYER_JOIN:
		return this->_handlePacket(packet.playerJoin, size);
	case Lobbies::OPCODE_PLAYER_LEAVE:
		return this->_handlePacket(packet.playerLeave, size);
	case Lobbies::OPCODE_KICKED:
		return this->_handlePacket(packet.kicked, size);
	case Lobbies::OPCODE_MOVE:
		return this->_handlePacket(packet.move, size);
	case Lobbies::OPCODE_POSITION:
		return this->_handlePacket(packet.position, size);
	case Lobbies::OPCODE_GAME_REQUEST:
		return this->_handlePacket(packet.gameRequest, size);
	case Lobbies::OPCODE_GAME_START:
		return this->_handlePacket(packet.gameStart, size);
	case Lobbies::OPCODE_PING:
		return this->_handlePacket(packet.ping, size);
	case Lobbies::OPCODE_PONG:
		return this->_handlePacket(packet.pong, size);
	case Lobbies::OPCODE_SETTINGS_UPDATE:
		return this->_handlePacket(packet.settingsUpdate, size);
	case Lobbies::OPCODE_ARCADE_ENGAGE:
		return this->_handlePacket(packet.arcadeEngage, size);
	case Lobbies::OPCODE_ARCADE_LEAVE:
		return this->_handlePacket(packet.arcadeLeave, size);
	case Lobbies::OPCODE_MESSAGE:
		return this->_handlePacket(packet.message, size);
	case Lobbies::OPCODE_IMPORTANT_MESSAGE:
		return this->_handlePacket(packet.importantMsg, size);
	case Lobbies::OPCODE_BATTLE_STATUS_UPDATE:
		return this->_handlePacket(packet.battleStatusUpdate, size);
	default:
		return this->error("Protocol error: Invalid opcode " + std::to_string(packet.opcode)), false;
	}
}

bool Connection::_handlePacket(const Lobbies::PacketHello &, size_t &)
{
	return this->error("Protocol error: OPCODE_HELLO unexpected"), false;
}

bool Connection::_handlePacket(const Lobbies::PacketOlleh &packet, size_t &size)
{
	if (size < sizeof(packet)) {
		print("Lobbies::PacketOlleh: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);

	Player player = this->_initParams;

	player.id = packet.id;
	player.name = std::string(packet.realName, strnlen(packet.realName, sizeof(packet.realName)));
	this->_players[packet.id] = player;
	this->_me = &this->_players[packet.id];
	this->_init = true;
	this->_helloPending = false;
	this->_spectatingArcade = false; // Machine membership belongs to the new lobby session.
	{ std::lock_guard<std::mutex> lock(_idleMutex); _idleClock.reset(IdleTracking::Clock::now()); }
	Lobbies::PacketMessage idleCapability{0, 0, "/__idlecap"};
	send(&idleCapability, sizeof(idleCapability));
	if (this->onConnect)
		this->onConnect(packet);
	return true;
}

bool Connection::_handlePacket(const Lobbies::PacketPlayerJoin &packet, size_t &size)
{
	if (!this->_init)
		return this->error("Protocol error: Invalid handshake"), false;
	if (size < sizeof(packet)) {
		print("Lobbies::PacketPlayerJoin: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);

	Player player;

	player.id = packet.id;
	player.name = std::string(packet.name, strnlen(packet.name, sizeof(packet.name)));
	player.player = packet.custom;
	this->_players[packet.id] = player;
	if (this->onPlayerJoin)
		this->onPlayerJoin(player);
	return true;
}

bool Connection::_handlePacket(const Lobbies::PacketPlayerLeave &packet, size_t &size)
{
	if (!this->_init)
		return this->error("Protocol error: Invalid handshake"), false;
	if (size < sizeof(packet)) {
		print("Lobbies::PacketPlayerLeave: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);
	if (packet.id == this->_me->id)
		return this->error("Protocol error: Server sent OPCODE_PLAYER_LEAVE with self id"), false;
	auto it = this->_players.find(packet.id);
	if (it == this->_players.end())
		return true;
	if (this->onPlayerLeave)
		this->onPlayerLeave(it->second);
	this->_players.erase(it);
	return true;
}

bool Connection::_handlePacket(const Lobbies::PacketKicked &packet, size_t &size)
{
	if (size < sizeof(packet)) {
		print("Lobbies::PacketKicked: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);

	std::string reason(packet.message, strnlen(packet.message, sizeof(packet.message)));
	constexpr char inactivePrefix[] = "Kicked for being inactive for ";
	constexpr char inactiveSuffix[] = " minutes.";
	bool inactiveAutoKick = false;

	if (
		reason.size() > sizeof(inactivePrefix) - 1 + sizeof(inactiveSuffix) - 1 &&
		reason.compare(0, sizeof(inactivePrefix) - 1, inactivePrefix) == 0 &&
		reason.compare(reason.size() - (sizeof(inactiveSuffix) - 1), sizeof(inactiveSuffix) - 1, inactiveSuffix) == 0
	) {
		auto minutesBegin = reason.begin() + sizeof(inactivePrefix) - 1;
		auto minutesEnd = reason.end() - (sizeof(inactiveSuffix) - 1);

		inactiveAutoKick = std::all_of(minutesBegin, minutesEnd, [](unsigned char c) {
			return c >= '0' && c <= '9';
		});
	}
	auto nowMs = steadyClockMilliseconds();
	auto lastSpectatingAtMs = this->_lastSpectatingAtMs.load();
	bool recentlySpectating = lastSpectatingAtMs > 0 && nowMs - lastSpectatingAtMs <= 10000;
	bool inSpectatingScene =
		SokuLib::sceneId == SokuLib::SCENE_LOADINGWATCH ||
		SokuLib::sceneId == SokuLib::SCENE_BATTLEWATCH ||
		SokuLib::newSceneId == SokuLib::SCENE_LOADINGWATCH ||
		SokuLib::newSceneId == SokuLib::SCENE_BATTLEWATCH;
	bool spectatorContext = this->_spectatingArcade.load() || this->_spectatingScene.load() || inSpectatingScene || recentlySpectating;
	if (!(spectatorContext && inactiveAutoKick) && this->onImpMsg)
		this->onImpMsg("Kicked: " + reason);
	this->_init = false;
	_receivedKick = true;
	_stopTransport();
	return true;
}

bool Connection::_handlePacket(const Lobbies::PacketMove &packet, size_t &size)
{
	if (!this->_init)
		return this->error("Protocol error: Invalid handshake"), false;
	if (size < sizeof(packet)) {
		print("Lobbies::PacketMove: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);
	auto player = this->_players.find(packet.id);
	if (player != this->_players.end())
		player->second.dir = packet.dir;
	return true;
}

bool Connection::_handlePacket(const Lobbies::PacketPosition &packet, size_t &size)
{
	if (!this->_init)
		return this->error("Protocol error: Invalid handshake"), false;
	if (size < sizeof(packet)) {
		print("Lobbies::PacketPosition: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);
	if (!validClientBattleStatus(packet.status)) return true;
	auto player = this->_players.find(packet.id);
	if (player != this->_players.end()) {
		player->second.pos = {packet.x, packet.y};
		player->second.battleStatus = packet.status;
		player->second.dir = packet.dir;
	}
	return true;
}

bool Connection::_handlePacket(const Lobbies::PacketGameRequest &packet, size_t &size)
{
	if (!this->_init)
		return this->error("Protocol error: Invalid handshake"), false;
	if (size < sizeof(packet)) {
		print("Lobbies::PacketGameRequest: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);
	return this->sendGameInfo();
}

bool Connection::_handlePacket(const Lobbies::PacketGameStart &packet, size_t &size)
{
	if (!this->_init)
		return this->error("Protocol error: Invalid handshake"), false;
	if (size < sizeof(packet)) {
		print("Lobbies::PacketGameStart: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);
	this->_spectatingArcade = packet.spectator;
	if (packet.spectator)
		this->_lastSpectatingAtMs = steadyClockMilliseconds();
	if (this->onConnectRequest){
		const char * ip = packet.ip;
		unsigned short port = packet.port;
		char ipv6[sizeof(packet.ipv6)+1];
		char ipv6MappedIpv4[16];
		if (isIpv6Available() && packet.port6){
			strncpy(ipv6, packet.ipv6, sizeof(packet.ipv6));
			ipv6[sizeof(ipv6)-1] = '\0';
			print("p1 ipv6 is: %s\n", ipv6);
			if (mapIpv6toIpv4(ipv6, ipv6MappedIpv4, sizeof(ipv6MappedIpv4))){
				ip = ipv6MappedIpv4;
				port = packet.port6;
			}
		}
		print("p1: %s:%u\n", ip, port);
		this->onConnectRequest(ip, port, packet.spectator);
	}
	return true;
}

bool Connection::_handlePacket(const Lobbies::PacketPing &, size_t &)
{
	return this->error("Protocol error: OPCODE_PING unexpected"), false;
}

bool Connection::_handlePacket(const Lobbies::PacketPong &packet, size_t &size)
{
	if (size < sizeof(packet)) {
		print("Lobbies::PacketPong: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);

	std::lock_guard<std::mutex> infoMutexLock(this->_infoMutex);
	this->_info.name = std::string(packet.name, strnlen(packet.name, sizeof(packet.name)));
	this->_info.maxPlayers = packet.maxPlayers;
	this->_info.currentPlayers = packet.currentPlayers;
	this->_info.hasPwd = packet.requiresPwd;
	return true;
}

bool Connection::_handlePacket(const Lobbies::PacketSettingsUpdate &packet, size_t &size)
{
	if (!this->_init)
		return this->error("Protocol error: Invalid handshake"), false;
	if (size < sizeof(packet)) {
		print("Lobbies::PacketSettingsUpdate: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);
	auto player = this->_players.find(packet.id);
	if (player != this->_players.end())
		player->second.player = packet.custom;
	return true;
}

bool Connection::_handlePacket(const Lobbies::PacketArcadeEngage &packet, size_t &size)
{
	if (!this->_init)
		return this->error("Protocol error: Invalid handshake"), false;
	if (size < sizeof(packet)) {
		print("Lobbies::PacketArcadeEngage: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);
	auto player = this->_players.find(packet.id);
	if (player == this->_players.end())
		return true;
	player->second.machineId = packet.machineId;
	if (this->onArcadeEngage)
		this->onArcadeEngage(player->second, packet.machineId);
	return true;
}

bool Connection::_handlePacket(const Lobbies::PacketArcadeLeave &packet, size_t &size)
{
	if (!this->_init)
		return this->error("Protocol error: Invalid handshake"), false;
	if (size < sizeof(packet)) {
		print("Lobbies::PacketArcadeLeave: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);
	if (this->_me && packet.id == this->_me->id) {
		this->_spectatingArcade = false;
		_gameInfoPending = false;
	}
	auto player = this->_players.find(packet.id);
	if (player == this->_players.end())
		return true;
	if (this->onArcadeLeave)
		this->onArcadeLeave(player->second, player->second.machineId);
	player->second.machineId = 0;
	return true;
}

void Connection::setSpectatingScene(bool spectating)
{
	this->_spectatingScene = spectating;
	if (spectating)
		this->_lastSpectatingAtMs = steadyClockMilliseconds();
}

bool Connection::_handlePacket(const Lobbies::PacketMessage &packet, size_t &size)
{
	if (size < sizeof(packet)) {
		print("Lobbies::PacketMessage: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);
	const std::string text(packet.message, strnlen(packet.message, sizeof(packet.message)));
	if (_init && packet.playerId == 0) {
		if (packet.channelId == Lobbies::BLOCKLIST_CONTROL_CHANNEL && text == "IDLECAP1") {
			std::lock_guard<std::mutex> lock(_idleMutex);
			_idleClock.supported = true;
			_idleClock.dirty = true;
			return true;
		}
		// Older servers reject the one-time capability query. Hide only its
		// specific unknown-command response, not ordinary errors or player text.
		if (text.find("__idlecap") != std::string::npos &&
			(text.find("Unknown command") != std::string::npos || text.find("未知指令") != std::string::npos))
			return true;
	}
	if (this->onMsg) {
		// The packet dispatcher already holds _playerMutex. Resolve and copy
		// the current name here so callbacks never need to lock it again.
		std::string senderName;
		if (packet.playerId != 0) {
			auto sender = this->_players.find(packet.playerId);
			if (sender != this->_players.end())
				senderName = sender->second.name;
		}
		this->onMsg(packet.channelId, packet.playerId, std::string(packet.message, strnlen(packet.message, sizeof(packet.message))), senderName);
	}
	return true;
}

bool Connection::_handlePacket(const Lobbies::PacketImportantMessage &packet, size_t &size)
{
	if (!this->_init)
		return this->error("Protocol error: Invalid handshake"), false;
	if (size < sizeof(packet)) {
		print("Lobbies::PacketImportantMessage: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);
	if (this->onImpMsg)
		this->onImpMsg(std::string(packet.message, strnlen(packet.message, sizeof(packet.message))));
	return true;
}

bool Connection::_handlePacket(const Lobbies::PacketBattleStatusUpdate &packet, size_t &size)
{
	if (!this->_init)
		return this->error("Protocol error: Invalid handshake"), false;
	if (size < sizeof(packet)) {
		print("Lobbies::PacketBattleStatusUpdate: size < sizeof(packet): %zu < %zu\n", size, sizeof(packet));
		return false;
	}
	size -= sizeof(packet);
	if (!validClientBattleStatus(packet.newStatus)) return true;
	auto player = this->_players.find(packet.playerId);
	if (player != this->_players.end())
		player->second.battleStatus = packet.newStatus;
	return true;
}

void Connection::connect()
{
	if (this->_init || !_connected || _helloPending.exchange(true)) return;
	_helloRequested = true;
}

void Connection::_sendHello()
{
	if (this->_init)
		return;

	unsigned char version[16];
	constexpr unsigned char rollNoSWRVersion[16] = {
		0x6F, 0x53, 0xD5, 0x29,
		0xFA, 0xC9, 0x60, 0x18,
		0x85, 0x9C, 0x21, 0xE2,
		0x71, 0x36, 0x70, 0x9F
	};
	Lobbies::Soku2VersionInfo soku2Info{soku2Major, soku2Minor, soku2Letter, soku2Force};

	// SokuRoll doesn't change the version string for non SWR linked so we replace it with something custom
	if (memcmp((unsigned char *)0x858B80, SokuLib::Soku110acRollSWRAllChars, 16) == 0 && SokuLib::SWRUnlinked)
		memcpy(version, rollNoSWRVersion, 16);
	else if (SokuLib::SWRUnlinked)
		memcpy(version, (unsigned char *)0x858B90, 16);
	else
		memcpy(version, (unsigned char *)0x858B80, 16);
	// Giuroll doesn't change the version string for non SWR linked either so we mirror the first byte
	if (
		memcmp((unsigned char *)0x858B81, SokuLib::Soku110acNoRollSWRAllChars + 1, 15) == 0 &&
		*(unsigned char *)0x858B80 != *SokuLib::Soku110acNoRollSWRAllChars
	)
		version[0] = *(unsigned char *)0x858B80;
	{
		std::lock_guard<std::mutex> lock(_sessionMutex);
		_helloSessions.push_back(_session.load());
	}

	if (this->_pwd) {
		Lobbies::PacketHello hello{soku2Info, version, this->_initParams.name, this->_initParams.player, this->_initParams.settings, *this->_pwd};

		this->send(&hello, sizeof(hello));
	} else {
		Lobbies::PacketHello hello{soku2Info, version, this->_initParams.name, this->_initParams.player, this->_initParams.settings};

		this->send(&hello, sizeof(hello));
	}
}

void Connection::disconnect()
{
	_gameInfoPending = false;
	if (!this->_init && !_helloPending)
		return;

	Lobbies::PacketPlayerLeave leave{0};

	// Stop heartbeat enqueueing before LEAVE; a later HELLO is a FIFO barrier.
	{ std::lock_guard<std::mutex> lock(_sendMutex); _init = false; _heartbeatReady = false; }
	this->send(&leave, sizeof(leave));
	_session.fetch_add(1); _helloPending = false; _helloRequested = false; _inbox.clear();
	_skippedChat = 0;
	{
		std::lock_guard<std::mutex> meMutexGuard(this->meMutex);
		std::lock_guard<std::mutex> playerMutexGuard(this->_playerMutex);
		std::lock_guard<std::mutex> functionMutexGuard(this->functionMutex);
		this->_init = false;
		this->_me = nullptr;
		this->_players.clear();
		_disconnectNotified = true;
		if (this->onDisconnect)
			this->onDisconnect();
	}
}

const Connection::LobbyInfo Connection::getLobbyInfo() const
{
	std::lock_guard<std::mutex> lock(_infoMutex);
	return this->_info;
}

Player *Connection::getMe()
{
	return this->_me;
}

const Player *Connection::getMe() const
{
	return this->_me;
}

std::vector<Player> Connection::getPlayers() const
{
	std::vector<Player> players;

	std::lock_guard<std::mutex> playerMutexGuard(this->_playerMutex);
	for (auto &p : this->_players)
		if (p.first && !p.second.name.empty())
			players.push_back(p.second);
	return players;
}

std::vector<std::string> Connection::getMessages() const
{
	this->_messagesMutex.lock();

	auto result = this->_messages;

	this->_messagesMutex.unlock();
	return result;
}

static void playerBasicAnimation(Player &player, const LobbyData::Avatar &avatar)
{
	char scale = 1 + ((player.dir & 0b100000) != 0);

	if (player.dir & 1)
		player.pos.x += PLAYER_H_SPEED * scale;
	if (player.dir & 2)
		player.pos.x -= PLAYER_H_SPEED * scale;
	player.animationCtr++;
	if (player.animationCtr > avatar.animationsStep) {
		player.currentAnimation++;
		player.currentAnimation %= avatar.nbAnimations;
		player.animationCtr = 0;
	}
	player.animation = (player.dir & 0b00011) != 0;
}

static void playerSuwakoAnimation(Player &player, const LobbyData::Avatar &avatar)
{
	char scale = 1 + ((player.dir & 0b100000) != 0);

	if (player.dir & 1)
		player.pos.x += PLAYER_H_SPEED * scale;
	if (player.dir & 2)
		player.pos.x -= PLAYER_H_SPEED * scale;
	player.animationCtr++;
	if (player.animationCtr > avatar.animationsStep) {
		player.currentAnimation++;
		player.currentAnimation %= avatar.nbAnimations;
		player.animationCtr = 0;
	}
	player.animation = (player.dir & 0b00011) != 0;
}

std::vector<std::function<void (Player &, const LobbyData::Avatar &)>> Connection::_playerUpdateHandles{
	playerBasicAnimation,
	playerSuwakoAnimation
};

void Connection::updatePlayers(const std::vector<LobbyData::Avatar> &avatars)
{
	this->_playerMutex.lock();
	for (auto &p : this->_players) {
		if (p.second.player.avatar >= avatars.size()) {
			char scale = 1 + ((p.second.dir & 0b100000) != 0);

			if (p.second.dir & 1)
				p.second.pos.x += PLAYER_H_SPEED * scale;
			if (p.second.dir & 2)
				p.second.pos.x -= PLAYER_H_SPEED * scale;
			p.second.animation = (p.second.dir & 0b00011) != 0;
			continue;
		}

		auto &avatar = avatars[p.second.player.avatar];

		Connection::_playerUpdateHandles[avatar.animationStyle](p.second, avatar);
	}
	this->_playerMutex.unlock();
}

void Connection::setPassword(const std::string &pwd)
{
	this->_pwd = pwd;
}

void Connection::setInitialPlayer(const Player &player)
{
	if (!_init && !_helloPending) _initParams = player;
}

bool Connection::sendGameInfo()
{
	if (!_init || !_connected || !_me) return false;
	// Capture the game callbacks now, on the game thread. The worker only does I/O.
	_gameInfoPort = onHostRequest();
	_gameInfoSession = _session.load();
	_gameInfoMachine = _me->machineId;
	_gameInfoPending = true;
	try {
		if (!_ipLookup.valid())
			_ipLookup = std::async(std::launch::async, [this] {
				auto ip = getMyIp(&_ipLookupAlive);
				return std::make_pair(std::move(ip), getMyIpv6());
			});
	} catch (const std::exception &e) {
		_gameInfoPending = false;
		error(std::string("Failed to get public IP: ") + e.what());
		return false;
	}
	return true;
}

void Connection::_finishGameInfo()
{
	if (!_ipLookup.valid() || _ipLookup.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	const bool publish = _gameInfoPending && _connected && _init && _me &&
		validClientGameInfoResult(_gameInfoSession, _session.load(), _gameInfoMachine, _me->machineId, _me->battleStatus);
	_gameInfoPending = false;
	try {
		auto addresses = _ipLookup.get();
		if (!publish) return;
		auto &ip = addresses.first;
		auto port = _gameInfoPort;
		const auto port6 = addresses.second.empty() ? 0 : port;
		const auto separator = ip.find(':');
		if (separator != std::string::npos) {
			try { port = static_cast<unsigned short>(std::stoul(ip.substr(separator + 1))); }
			catch (const std::exception &e) { puts(e.what()); }
			ip.resize(separator);
		}
		Lobbies::PacketGameStart game{ip.c_str(), port, port6 ? addresses.second : "", static_cast<unsigned short>(port6), false};
		send(&game, sizeof(game));
	} catch (const std::exception &e) {
		if (publish) error(std::string("Failed to get public IP: ") + e.what());
	} catch (...) {
		if (publish) error("Failed to get public IP: Unknown IP lookup error");
	}
}
