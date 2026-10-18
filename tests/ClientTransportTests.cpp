#include "ClientTransport.hpp"
#include "ClientChatLog.hpp"
#include "Socket.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif

static void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }

static void inboxBoundsAndOwnership() {
	ClientInbox inbox; std::atomic_bool running{true}; char text[] = "original";
	require(!inbox.push(text, ClientInbox::MAX_BYTES + 1, 7, false, running), "oversized frame did not fail immediately");
	require(inbox.push(text, sizeof(text), 7, false, running), "enqueue failed");
	text[0] = '!'; ClientInbox::Frame frame;
	require(inbox.pop(frame) && frame.session == 7 && frame.bytes[0] == 'o', "queued frame aliases producer memory");
	for (size_t i = 0; i < ClientInbox::MAX_CHAT + 10; ++i) inbox.push(text, sizeof(text), 7, true, running);
	require(inbox.takeDropped() == 10, "chat bound not enforced");
	require(inbox.push(text, sizeof(text), 7, false, running), "chat consumed reliable capacity");
	size_t count = 0; while (inbox.pop(frame)) ++count;
	require(count == ClientInbox::MAX_CHAT + 1, "reliable frame lost under chat flood");
	for (size_t i = 0; i < ClientInbox::MAX_FRAMES; ++i) {
		const char value = static_cast<char>(i); inbox.push(&value, 1, i, false, running);
	}
	auto producer = std::async(std::launch::async, [&] { return inbox.push(text, sizeof(text), 9999, false, running); });
	require(producer.wait_for(std::chrono::milliseconds(40)) == std::future_status::timeout, "full reliable queue failed to pause ingress");
	require(inbox.pop(frame) && frame.session == 0, "FIFO first frame incorrect");
	require(producer.wait_for(std::chrono::seconds(2)) == std::future_status::ready && producer.get(), "capacity wake lost");
	for (size_t i = 1; i < ClientInbox::MAX_FRAMES; ++i)
		require(inbox.pop(frame) && frame.session == i, "reliable events reordered");
	require(inbox.pop(frame) && frame.session == 9999, "retained frame lost after pressure");
	inbox.clear();
	for (size_t i = 0; i < ClientInbox::MAX_FRAMES; ++i) inbox.push(text, sizeof(text), 8, false, running);
	auto cancelled = std::async(std::launch::async, [&] { return inbox.push(text, sizeof(text), 8, false, running); });
	running = false; inbox.wake();
	require(cancelled.wait_for(std::chrono::seconds(2)) == std::future_status::ready && !cancelled.get(), "shutdown failed to release paused ingress");
	inbox.clear(); require(inbox.empty(), "session clear retained events");
	std::cout << "PASS immutable FIFO, chat reserve, capacity wake, cancellation and clear\n";
}

static void nonblockingConnect() {
	Socket client; std::atomic_bool running{true};
	const auto listener = ::socket(AF_INET, SOCK_STREAM, 0);
	require(listener != INVALID_SOCKET, "test listener creation failed");
	SOCKET peer = INVALID_SOCKET;
	auto cleanup = [&] {
#ifdef _WIN32
		if (peer != INVALID_SOCKET) closesocket(peer); closesocket(listener);
#else
		if (peer != INVALID_SOCKET) ::close(peer); ::close(listener);
#endif
	};
	try {
		sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		require(::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 && ::listen(listener, 1) == 0, "test listener bind failed");
#ifdef _WIN32
		int length = sizeof(address);
#else
		socklen_t length = sizeof(address);
#endif
		require(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0, "listener address unavailable");
		client.connectWithTimeout("127.0.0.1", ntohs(address.sin_port), std::chrono::seconds(2), running);
		require(client.isOpen(), "timed connect failed"); peer = ::accept(listener, nullptr, nullptr);
		require(peer != INVALID_SOCKET, "test accept failed");
		char byte; const auto result = ::recv(client.getSockFd(), &byte, 1, 0);
#ifdef _WIN32
		require(result < 0 && WSAGetLastError() == WSAEWOULDBLOCK, "client socket was left blocking");
#else
		require(result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK), "client socket was left blocking");
#endif
		Socket cancelled; running = false;
		cancelled.connectWithTimeout("cancelled.invalid", 1, std::chrono::seconds(2), running);
		require(!cancelled.isOpen(), "cancelled connect published a socket");
	} catch (...) { cleanup(); throw; }
	cleanup(); std::cout << "PASS loopback nonblocking connect and pre-connect cancellation\n";
}

static void chatLogFile() {
	const auto directory = std::filesystem::temp_directory_path() /
		("sokulobbies-client-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	queueChatLog(directory, "sample\none\rline"); queueChatLog(directory, "second");
	std::filesystem::path file;
	bool found = false;
	for (unsigned i = 0; i < 200 && !found; ++i) {
		if (std::filesystem::exists(directory)) for (const auto &entry : std::filesystem::directory_iterator(directory)) {
			file = entry.path(); std::ifstream input(file); std::string contents((std::istreambuf_iterator<char>(input)), {});
			found = contents.find("sample one line\n") != std::string::npos && contents.find("second\n") != std::string::npos;
		}
		if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	require(found, "asynchronous chat log did not persist ordered sanitized lines");
	std::filesystem::remove(file); std::filesystem::remove(directory);
	std::cout << "PASS asynchronous chat file and newline sanitization\n";
}

int main() {
	try {
		require(clientPacketSize(Lobbies::OPCODE_OLLEH) == sizeof(Lobbies::PacketOlleh) &&
			clientPacketSize(Lobbies::OPCODE_MESSAGE) == sizeof(Lobbies::PacketMessage) && clientPacketSize(255) == 0,
			"legacy frame sizes changed");
		inboxBoundsAndOwnership(); nonblockingConnect(); chatLogFile();
		return 0;
	} catch (const std::exception &error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
