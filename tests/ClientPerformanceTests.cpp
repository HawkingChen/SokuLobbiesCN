#include "ClientPerformance.hpp"
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>

namespace ClientPerformance { std::string testThreadProbe(); }
namespace ClientRenderDiagnostics { bool testRenderHooks(); }

int main() {
	try {
		std::atomic<bool> stop{false};
		std::thread idle([&] { while (!stop.load()) Sleep(1); });
		std::string probe;
		try { probe = ClientPerformance::testThreadProbe(); }
		catch (...) { stop.store(true); idle.join(); throw; }
		stop.store(true); idle.join();
		if (probe.find("thread_probe capture=1") == std::string::npos ||
			probe.find("thread id=") == std::string::npos || probe.find(" ip=") == std::string::npos)
			throw std::runtime_error("thread context sampling failed");
		const auto folder = std::filesystem::temp_directory_path() /
			("sokulobbies-perf-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
		std::filesystem::create_directory(folder);
		const auto file = folder / L"lobby-performance-test.log";
		const auto previous = folder / L"lobby-performance-test.previous.log";
		{
			std::ofstream oversized(file, std::ios::binary);
			oversized.seekp(8 * 1024 * 1024); oversized.put('x');
		}
		using namespace ClientPerformance;
		tick(folder.c_str(), 4242, true, "smoke");
		if (!ClientRenderDiagnostics::testRenderHooks()) throw std::runtime_error("render hook forwarding/coverage failed");
		record(Stage::LobbyRender, 12345); record(Stage::LobbyRender, 567);
		record(Stage::Present, 2000); record(Stage::RenderGap, 200000);
		{ Scope phase(Stage::TextCreate); phase.next(Stage::TextGetDC); }
		gauge(Gauge::Players, 42); gauge(Gauge::RatingTextures, 17);
		texture(true); texture(false);
		std::this_thread::sleep_for(std::chrono::milliseconds(5050));
		tick(folder.c_str(), 4242, true, "smoke");
		bool found = false;
		for (unsigned i = 0; i < 100 && !found; ++i) {
			std::ifstream input(file, std::ios::binary);
			std::string content((std::istreambuf_iterator<char>(input)), {});
			found = content.find("perf-v1 Release version=smoke") != std::string::npos &&
				content.find("scene=4242 lobby=1") != std::string::npos &&
				content.find("lobby_render=2/6456/12345/12912") != std::string::npos &&
				content.find("diagnostic_revision=4") != std::string::npos &&
				content.find("render_results swap_present ok=2 still_drawing=1 device_lost=1 not_reset=1 invalid_call=1 other_failure=1") != std::string::npos &&
				content.find("render_results reset ok=1") != std::string::npos &&
				content.find("render_results reset_ex ok=0 still_drawing=0 device_lost=1") != std::string::npos &&
				content.find("last_hresult=0x88760868") != std::string::npos &&
				content.find("last_scene=4321") != std::string::npos &&
				content.find("render_gap=1/200000/200000/200000") != std::string::npos &&
				content.find("text_create=1/") != std::string::npos && content.find("text_getdc=1/") != std::string::npos &&
				content.find("players=42") != std::string::npos && content.find("rating_textures=17") != std::string::npos &&
				content.find("texture_created_total=1 texture_failed_total=1") != std::string::npos &&
				content.find("private_kib=") != std::string::npos;
			if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		if (!found) throw std::runtime_error("performance log missing or incorrect");
		if (!std::filesystem::exists(previous) || std::filesystem::file_size(previous) <= 8 * 1024 * 1024)
			throw std::runtime_error("performance log rotation failed");
		std::filesystem::remove(file); std::filesystem::remove(previous); std::filesystem::remove(folder);
		std::cout << "PASS asynchronous Release performance file, rotation, interval aggregates, resource snapshots and shutdown\n";
		return 0;
	} catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
