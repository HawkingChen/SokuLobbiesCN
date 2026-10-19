#include "ClientPerformance.hpp"

#ifdef SOKU_CLIENT_PERFORMANCE
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <map>

namespace ClientPerformance {
namespace {
	constexpr size_t stages = static_cast<size_t>(Stage::Count), gauges = static_cast<size_t>(Gauge::Count);
	const char *stageNames[] = {"update_hook", "original_update", "end_scene", "lobby_update", "lobby_render",
		"network_pump", "texture_work", "chat_render", "name_render", "bubble_render", "rating_cards", "rating_lookup", "rating_batch", "text_texture", "snapshot",
		"present", "text_create", "text_getdc", "text_gdi", "text_lock", "text_pixels", "text_upload", "render_gap",
		"swap_present", "present_ex", "reset", "reset_ex", "device_sample"};
	const char *gaugeNames[] = {"players", "chat_messages", "chat_fragments", "name_textures", "text_bubbles", "emote_bubbles",
		"rating_cards", "pending_names", "pending_bubbles", "pending_removals", "inbox_frames", "inbox_bytes",
		"send_bytes", "connections", "rating_pending", "rating_cache", "pending_name_queue", "pending_bubble_queue", "rating_textures",
		"present_hook", "device_state_hresult_u32", "software_vp", "device_flags", "viewport_width", "viewport_height", "text_d3d9ex",
		"swap_present_hook", "reset_hook", "present_ex_hook", "reset_ex_hook", "device_epoch", "swap_epoch",
		"swap_effect", "back_buffers", "presentation_interval", "windowed"};
	static_assert(sizeof(stageNames) / sizeof(*stageNames) == stages, "stage label mismatch");
	static_assert(sizeof(gaugeNames) / sizeof(*gaugeNames) == gauges, "gauge label mismatch");
	struct Stat { std::atomic<uint64_t> calls{0}, total{0}, maximum{0}; };
	std::array<Stat, stages> stats;
	std::array<std::atomic<uint64_t>, gauges> values{};
	std::atomic<uint64_t> textureOK{0}, textureFailed{0};
	struct RenderStat {
		std::atomic<uint64_t> ok{0}, stillDrawing{0}, lost{0}, notReset{0}, invalid{0}, other{0}, inFlight{0}, streak{0}, peakStreak{0};
		std::atomic<uint32_t> lastResult{0}, thread{0}, flags{0}, scene{0};
		std::atomic<uint64_t> startedAt{0};
	};
	std::array<RenderStat, static_cast<size_t>(RenderCall::Count)> renderStats;
	const char *renderNames[] = {"device_present", "swap_present", "present_ex", "reset", "reset_ex"};
	static_assert(sizeof(renderNames) / sizeof(*renderNames) == static_cast<size_t>(RenderCall::Count), "render label mismatch");
	void maximum(std::atomic<uint64_t> &dest, uint64_t value) noexcept {
		auto old = dest.load(std::memory_order_relaxed);
		while (old < value && !dest.compare_exchange_weak(old, value, std::memory_order_relaxed)) {}
	}
	struct Handle {
		HANDLE value;
		~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
	};
	uint64_t ticksOf(const FILETIME &time) {
		return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
	}
	class ThreadProbe {
		std::map<DWORD, uint64_t> _previous;
		unsigned _captures = 0;
		struct Module { uintptr_t begin, end; std::string name; };
		std::vector<Module> modules() {
			std::vector<Module> result;
			Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId())};
			MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
			if (snapshot.value != INVALID_HANDLE_VALUE && Module32FirstW(snapshot.value, &entry)) do {
				auto begin = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
				char name[1024]{};
				WideCharToMultiByte(CP_UTF8, 0, entry.szModule, -1, name, sizeof(name), nullptr, nullptr);
				result.push_back({begin, begin + entry.modBaseSize, name});
			} while (result.size() < 512 && Module32NextW(snapshot.value, &entry));
			return result;
		}
		static std::string address(uintptr_t value, const std::vector<Module> &list) {
			char text[100];
			for (const auto &module : list) if (value >= module.begin && value < module.end) {
				snprintf(text, sizeof(text), "+0x%llx", static_cast<uint64_t>(value - module.begin));
				return module.name + text;
			}
			// Do not dump arbitrary stack contents or unknown pointers.
			return {};
		}
	public:
		std::string collect(bool trigger) {
			struct Thread { DWORD id; uint64_t delta; };
			std::vector<Thread> threads;
			std::map<DWORD, uint64_t> current;
			Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)};
			THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
			if (snapshot.value != INVALID_HANDLE_VALUE && Thread32First(snapshot.value, &entry)) do {
				if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
				Handle thread{OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID)};
				FILETIME created{}, exited{}, kernel{}, user{};
				if (thread.value && GetThreadTimes(thread.value, &created, &exited, &kernel, &user)) {
					auto cpu = ticksOf(kernel) + ticksOf(user);
					auto old = _previous.find(entry.th32ThreadID);
					threads.push_back({entry.th32ThreadID, old != _previous.end() && cpu >= old->second ? cpu - old->second : 0});
					current[entry.th32ThreadID] = cpu;
				}
			} while (threads.size() < 128 && Thread32Next(snapshot.value, &entry));
			_previous.swap(current);
			if (!trigger || _captures >= 12) return {};
			++_captures;
			auto list = modules();
			std::string output = "thread_probe capture=" + std::to_string(_captures) + " (worker sampling; CPU delta since previous summary; stack candidates are NOT an unwound call stack)\nmodules:";
			for (const auto &module : list) output += " " + module.name;
			output += '\n';
			std::sort(threads.begin(), threads.end(), [](const Thread &a, const Thread &b) { return a.delta > b.delta; });
			for (size_t i = 0; i < (std::min)(threads.size(), size_t{3}); ++i) {
				output += "thread id=" + std::to_string(threads[i].id) + " cpu_delta_us=" + std::to_string(threads[i].delta / 10);
#ifdef _M_IX86
				Handle thread{OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, threads[i].id)};
				CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL;
				std::array<uintptr_t, 256> stack{}; SIZE_T bytes = 0;
				bool obtained = false;
				if (thread.value && SuspendThread(thread.value) != static_cast<DWORD>(-1)) {
					// No allocation, formatting, symbol loading or application locks while
					// suspended. Resume before touching the logger or module list.
					obtained = GetThreadContext(thread.value, &context) != FALSE;
					if (obtained) ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(context.Esp), stack.data(), sizeof(stack), &bytes);
					ResumeThread(thread.value);
				}
				if (obtained) {
					output += " ip=" + address(context.Eip, list) + " stack_module_candidates:";
					unsigned count = 0;
					for (size_t n = 0; n < bytes / sizeof(uintptr_t) && count < 20; ++n) {
						auto label = address(stack[n], list);
						if (!label.empty()) { output += " " + label; ++count; }
					}
				} else output += " context_unavailable";
#else
				output += " context_sampling_requires_x86";
#endif
				output += '\n';
			}
			return output + '\n';
		}
	};
	class Writer {
		std::mutex _mutex;
		std::condition_variable _wake;
		std::deque<std::pair<std::string, bool>> _queue;
		std::filesystem::path _path;
		bool _stop = false;
		std::thread _thread;
		void run() noexcept {
			try {
				ThreadProbe probe;
				for (;;) {
					std::string entry;
					bool trigger = false;
					{
						std::unique_lock<std::mutex> lock(_mutex);
						_wake.wait(lock, [&] { return _stop || !_queue.empty(); });
						if (_queue.empty()) return;
						entry = std::move(_queue.front().first); trigger = _queue.front().second; _queue.pop_front();
					}
					try { entry += probe.collect(trigger); } catch (...) { entry += "thread_probe failed\n"; }
					std::error_code error;
					const auto size = std::filesystem::file_size(_path, error);
					bool truncate = false;
					if (!error && size > 8 * 1024 * 1024) {
						auto previous = _path; previous.replace_filename(L"lobby-performance-test.previous.log");
						// Recoverable rotation; only these two diagnostic files are replaced.
						truncate = !MoveFileExW(_path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
					}
					if (FILE *file = _wfopen(_path.c_str(), truncate ? L"wb" : L"ab")) {
						fwrite(entry.data(), 1, entry.size(), file); fclose(file);
					}
				}
			} catch (...) {}
		}
	public:
		explicit Writer(const wchar_t *folder) : _path(std::filesystem::path(folder) / L"lobby-performance-test.log"), _thread([this] { run(); }) {}
		~Writer() noexcept {
			{ std::lock_guard<std::mutex> lock(_mutex); _stop = true; }
			_wake.notify_all();
			if (_thread.joinable()) _thread.join();
		}
		void push(std::string entry, bool trigger) {
			{ std::lock_guard<std::mutex> lock(_mutex); if (_queue.size() >= 8) return; _queue.emplace_back(std::move(entry), trigger); }
			_wake.notify_one();
		}
	};
	uint64_t fileTime(const FILETIME &value) { return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; }
}

#ifdef SOKU_PERFORMANCE_TESTS
std::string testThreadProbe() { ThreadProbe probe; probe.collect(false); return probe.collect(true); }
#endif

void record(Stage stage, uint64_t microseconds) noexcept {
	auto &stat = stats[static_cast<size_t>(stage)];
	stat.calls.fetch_add(1, std::memory_order_relaxed);
	stat.total.fetch_add(microseconds, std::memory_order_relaxed);
	maximum(stat.maximum, microseconds);
}
void gauge(Gauge which, uint64_t value) noexcept { values[static_cast<size_t>(which)].store(value, std::memory_order_relaxed); }
void texture(bool succeeded) noexcept { (succeeded ? textureOK : textureFailed).fetch_add(1, std::memory_order_relaxed); }
void renderBegin(RenderCall call, unsigned scene) noexcept {
	auto &stat = renderStats[static_cast<size_t>(call)];
	stat.thread.store(GetCurrentThreadId()); stat.scene.store(scene); stat.startedAt.store(GetTickCount64());
	stat.inFlight.fetch_add(1, std::memory_order_relaxed);
}
void renderEnd(RenderCall call, int32_t result, uint32_t flags, unsigned scene) noexcept {
	auto &stat = renderStats[static_cast<size_t>(call)];
	const auto code = static_cast<uint32_t>(result);
	stat.lastResult.store(code); stat.flags.store(flags); stat.thread.store(GetCurrentThreadId()); stat.scene.store(scene);
	if (result >= 0) { stat.ok.fetch_add(1); stat.streak.store(0); }
	else {
		switch (code) {
		case 0x8876021c: stat.stillDrawing.fetch_add(1); break; // D3DERR_WASSTILLDRAWING
		case 0x88760868: stat.lost.fetch_add(1); break;
		case 0x88760869: stat.notReset.fetch_add(1); break;
		case 0x8876086c: stat.invalid.fetch_add(1); break;
		default: stat.other.fetch_add(1); break;
		}
		maximum(stat.peakStreak, stat.streak.fetch_add(1) + 1);
	}
	stat.inFlight.fetch_sub(1, std::memory_order_relaxed);
}
bool snapshotDue(uint64_t &last) noexcept {
	const auto now = GetTickCount64();
	if (last && now - last < 5000) return false;
	last = now; return true;
}

void tick(const wchar_t *folder, unsigned scene, bool lobby, const char *version) noexcept {
	// Called only by the game thread; worker contributions are atomic snapshots.
	static uint64_t last = 0, previousTick = 0, gapMax = 0, slow50 = 0, ticks = 0, previousCPU = 0;
	const auto now = GetTickCount64();
	if (previousTick) { const auto gap = now - previousTick; gapMax = (std::max)(gapMax, gap); if (gap >= 50) ++slow50; }
	previousTick = now; ++ticks;
	if (last && now - last < 5000) return;
	const auto period = last ? now - last : 0; last = now;
	try {
		if (!folder || !*folder) return;
		static Writer writer(folder);
		SYSTEMTIME time{}; GetLocalTime(&time);
		DWORD foregroundPID = 0; GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPID);
		PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
		GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory));
		DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
		FILETIME created{}, exited{}, kernel{}, user{};
		GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
		const auto cpu = fileTime(kernel) + fileTime(user);
		const double cpuPercent = period && previousCPU ? (cpu - previousCPU) / (period * 100.0) : 0;
		previousCPU = cpu;
		char line[900];
		snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u perf-v1 Release version=%s pid=%lu scene=%u lobby=%u foreground=%u period_ms=%llu update_hz=%.1f gap_max_ms=%llu gaps_ge50=%llu cpu_core_pct=%.1f private_kib=%llu working_kib=%llu handles=%lu gdi=%lu user=%lu\n",
			time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, version, GetCurrentProcessId(), scene, lobby,
			foregroundPID == GetCurrentProcessId(), period,
			period ? ticks * 1000.0 / period : 0, gapMax, slow50, cpuPercent,
			static_cast<uint64_t>(memory.PrivateUsage / 1024), static_cast<uint64_t>(memory.WorkingSetSize / 1024), handles,
			GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
		std::string message(line);
		message += "diagnostic_revision=4 present=device+swapchain+PresentEx; Reset+ResetEx; render_gap=EndScene_entry_interval_us; no_direct_frameskip_flag_probe\n";
		const auto renderCalls = stats[static_cast<size_t>(Stage::EndScene)].calls.load();
		const bool probeTrigger = period && cpuPercent >= 80 && renderCalls * 1000.0 / period < 30;
		snprintf(line, sizeof(line), "hook_rates_hz lobby_render=%.1f end_scene=%.1f\n",
			period ? stats[static_cast<size_t>(Stage::LobbyRender)].calls.load() * 1000.0 / period : 0,
			period ? stats[static_cast<size_t>(Stage::EndScene)].calls.load() * 1000.0 / period : 0);
		message += line;
		message += "timings_us_inclusive=count/avg/max/total (nested timers overlap; rating_batch includes network wait)\n";
		for (size_t i = 0; i < stages; ++i) {
			const auto count = stats[i].calls.exchange(0), total = stats[i].total.exchange(0), peak = stats[i].maximum.exchange(0);
			snprintf(line, sizeof(line), " %s=%llu/%llu/%llu/%llu", stageNames[i], count, count ? total / count : 0, peak, total);
			message += line;
		}
		message += '\n';
		for (size_t i = 0; i < renderStats.size(); ++i) {
			auto &r = renderStats[i];
			const auto inFlight = r.inFlight.load(), startedAt = r.startedAt.load();
			const auto observedAt = GetTickCount64();
			snprintf(line, sizeof(line), "render_results %s ok=%llu still_drawing=%llu device_lost=%llu not_reset=%llu invalid_call=%llu other_failure=%llu in_flight=%llu consecutive_failures=%llu peak_consecutive_failures=%llu last_hresult=0x%08x last_flags=0x%08x last_thread=%u last_scene=%u active_age_ms=%llu\n",
				renderNames[i], r.ok.exchange(0), r.stillDrawing.exchange(0), r.lost.exchange(0), r.notReset.exchange(0), r.invalid.exchange(0), r.other.exchange(0),
				inFlight, r.streak.load(), r.peakStreak.exchange(0), r.lastResult.load(), r.flags.load(), r.thread.load(), r.scene.load(),
				inFlight && startedAt && observedAt >= startedAt ? observedAt - startedAt : 0);
			message += line;
		}
		message += "gauges (latest snapshot; values outside lobby may be stale):";
		for (size_t i = 0; i < gauges; ++i) { snprintf(line, sizeof(line), " %s=%llu", gaugeNames[i], values[i].load()); message += line; }
		snprintf(line, sizeof(line), " texture_created_total=%llu texture_failed_total=%llu\n\n", textureOK.load(), textureFailed.load());
		message += line; writer.push(std::move(message), probeTrigger);
	} catch (...) {}
	ticks = gapMax = slow50 = 0;
}
}
#endif
