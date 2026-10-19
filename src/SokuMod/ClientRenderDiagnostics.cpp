#include "ClientRenderDiagnostics.hpp"

#ifdef SOKU_CLIENT_PERFORMANCE
#include "ClientPerformance.hpp"
#include <Windows.h>
#include <d3d9.h>
#include <array>
#include <atomic>

namespace ClientRenderDiagnostics {
namespace {
using namespace ClientPerformance;
std::atomic<unsigned> currentScene{0};

// Only the game update thread installs hooks. Calls on the presenting thread
// use a fixed, published registry: no allocation, mutexes or logging in hooks.
// Keep prior entries when a device/swap chain is recreated. Do not overwrite
// another mod's later hook, and never change flags, parameters or return codes.
template<typename Function, size_t Index> class Hook {
	struct Entry { std::atomic<void **> table{nullptr}; Function original = nullptr; };
	std::array<Entry, 16> _entries{};
public:
	bool install(void *object, Function replacement) noexcept {
		auto table = *reinterpret_cast<void ***>(object);
		Entry *empty = nullptr;
		for (auto &entry : _entries) {
			auto bound = entry.table.load(std::memory_order_acquire);
			if (bound == table) return table[Index] == reinterpret_cast<void *>(replacement);
			if (!bound && !empty) empty = &entry;
		}
		if (!empty || !table[Index] || table[Index] == reinterpret_cast<void *>(replacement)) return false;
		DWORD protection;
		if (!VirtualProtect(&table[Index], sizeof(void *), PAGE_EXECUTE_READWRITE, &protection)) return false;
		empty->original = reinterpret_cast<Function>(table[Index]);
		empty->table.store(table, std::memory_order_release);
		InterlockedExchangePointer(reinterpret_cast<void *volatile *>(&table[Index]), reinterpret_cast<void *>(replacement));
		DWORD unused; VirtualProtect(&table[Index], sizeof(void *), protection, &unused);
		return true;
	}
	Function original(void *object) const noexcept {
		auto table = *reinterpret_cast<void ***>(object);
		for (const auto &entry : _entries)
			if (entry.table.load(std::memory_order_acquire) == table) return entry.original;
		// A third-party proxy can clone an already hooked vtable. If all known
		// implementations share one target it is still unambiguous to forward.
		Function fallback = nullptr;
		for (const auto &entry : _entries) if (entry.table.load(std::memory_order_acquire)) {
			if (fallback && fallback != entry.original) return nullptr;
			fallback = entry.original;
		}
		return fallback;
	}
};

using DevicePresent = HRESULT (WINAPI *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
using SwapPresent = HRESULT (WINAPI *)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
using ExPresent = HRESULT (WINAPI *)(IDirect3DDevice9Ex *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD);
using DeviceReset = HRESULT (WINAPI *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
using ExReset = HRESULT (WINAPI *)(IDirect3DDevice9Ex *, D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *);
Hook<DevicePresent, 17> devicePresent;
Hook<SwapPresent, 3> swapPresent;
Hook<ExPresent, 121> exPresent;
Hook<DeviceReset, 16> deviceReset;
Hook<ExReset, 132> exReset;

class Call {
	RenderCall _call;
	Scope _timer;
public:
	Call(RenderCall call, Stage stage) noexcept : _call(call), _timer(stage) { renderBegin(call, currentScene.load(std::memory_order_relaxed)); }
	HRESULT finish(HRESULT result, DWORD flags = 0) noexcept {
		renderEnd(_call, result, flags, currentScene.load(std::memory_order_relaxed));
		return result;
	}
};

HRESULT WINAPI onDevicePresent(IDirect3DDevice9 *device, const RECT *source, const RECT *dest, HWND window, const RGNDATA *dirty)
{
	Call call(RenderCall::DevicePresent, Stage::Present);
	auto original = devicePresent.original(device);
	return call.finish(original ? original(device, source, dest, window, dirty) : D3DERR_INVALIDCALL);
}
HRESULT WINAPI onSwapPresent(IDirect3DSwapChain9 *chain, const RECT *source, const RECT *dest, HWND window, const RGNDATA *dirty, DWORD flags)
{
	Call call(RenderCall::SwapPresent, Stage::SwapPresent);
	auto original = swapPresent.original(chain);
	return call.finish(original ? original(chain, source, dest, window, dirty, flags) : D3DERR_INVALIDCALL, flags);
}
HRESULT WINAPI onExPresent(IDirect3DDevice9Ex *device, const RECT *source, const RECT *dest, HWND window, const RGNDATA *dirty, DWORD flags)
{
	Call call(RenderCall::PresentEx, Stage::PresentEx);
	auto original = exPresent.original(device);
	return call.finish(original ? original(device, source, dest, window, dirty, flags) : D3DERR_INVALIDCALL, flags);
}
HRESULT WINAPI onReset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *parameters)
{
	Call call(RenderCall::Reset, Stage::Reset);
	auto original = deviceReset.original(device);
	return call.finish(original ? original(device, parameters) : D3DERR_INVALIDCALL);
}
HRESULT WINAPI onResetEx(IDirect3DDevice9Ex *device, D3DPRESENT_PARAMETERS *parameters, D3DDISPLAYMODEEX *mode)
{
	Call call(RenderCall::ResetEx, Stage::ResetEx);
	auto original = exReset.original(device);
	return call.finish(original ? original(device, parameters, mode) : D3DERR_INVALIDCALL);
}
}

void sample(IDirect3DDevice9 *device, unsigned scene) noexcept
{
	currentScene.store(scene, std::memory_order_relaxed);
	static ULONGLONG previousSample = 0;
	const auto now = GetTickCount64();
	if (!device || (previousSample && now - previousSample < 250)) return;
	previousSample = now;
	Scope timer(Stage::DeviceSample);
	static uintptr_t previousDevice = 0, previousChain = 0;
	static uint64_t deviceEpoch = 0, chainEpoch = 0;
	if (reinterpret_cast<uintptr_t>(device) != previousDevice) { previousDevice = reinterpret_cast<uintptr_t>(device); ++deviceEpoch; }
	gauge(Gauge::DeviceEpoch, deviceEpoch);
	gauge(Gauge::PresentHook, devicePresent.install(device, onDevicePresent));
	gauge(Gauge::ResetHook, deviceReset.install(device, onReset));
	IDirect3DDevice9Ex *extended = nullptr;
	if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void **>(&extended)))) {
		gauge(Gauge::PresentExHook, exPresent.install(extended, onExPresent));
		gauge(Gauge::ResetExHook, exReset.install(extended, onResetEx));
		extended->Release();
	} else { gauge(Gauge::PresentExHook, 0); gauge(Gauge::ResetExHook, 0); }
	IDirect3DSwapChain9 *chain = nullptr;
	if (SUCCEEDED(device->GetSwapChain(0, &chain))) {
		if (reinterpret_cast<uintptr_t>(chain) != previousChain) { previousChain = reinterpret_cast<uintptr_t>(chain); ++chainEpoch; }
		gauge(Gauge::SwapEpoch, chainEpoch);
		gauge(Gauge::SwapPresentHook, swapPresent.install(chain, onSwapPresent));
		D3DPRESENT_PARAMETERS parameters{};
		if (SUCCEEDED(chain->GetPresentParameters(&parameters))) {
			gauge(Gauge::SwapEffect, parameters.SwapEffect); gauge(Gauge::BackBuffers, parameters.BackBufferCount);
			gauge(Gauge::PresentationInterval, parameters.PresentationInterval); gauge(Gauge::Windowed, parameters.Windowed);
		}
		chain->Release();
	} else gauge(Gauge::SwapPresentHook, 0);
	static uint64_t last = 0;
	if (!snapshotDue(last)) return;
	gauge(Gauge::DeviceState, static_cast<uint32_t>(device->TestCooperativeLevel()));
	gauge(Gauge::SoftwareVP, device->GetSoftwareVertexProcessing());
	D3DDEVICE_CREATION_PARAMETERS parameters{};
	if (SUCCEEDED(device->GetCreationParameters(&parameters))) gauge(Gauge::DeviceFlags, parameters.BehaviorFlags);
	D3DVIEWPORT9 viewport{};
	if (SUCCEEDED(device->GetViewport(&viewport))) { gauge(Gauge::ViewportWidth, viewport.Width); gauge(Gauge::ViewportHeight, viewport.Height); }
}

#ifdef SOKU_PERFORMANCE_TESTS
namespace {
HRESULT testResult = S_OK;
DWORD testFlags = 0;
const RECT *testSource = nullptr;
const RECT *testDest = nullptr;
HWND testWindow = nullptr;
const RGNDATA *testDirty = nullptr;
D3DPRESENT_PARAMETERS *testParameters = nullptr;
D3DDISPLAYMODEEX *testMode = nullptr;
unsigned testCalls = 0;
HRESULT WINAPI fakeSwap(IDirect3DSwapChain9 *, const RECT *source, const RECT *dest, HWND window, const RGNDATA *dirty, DWORD flags) {
	++testCalls; testFlags = flags; testSource = source; testDest = dest; testWindow = window; testDirty = dirty; return testResult;
}
HRESULT WINAPI fakeDevice(IDirect3DDevice9 *, const RECT *source, const RECT *dest, HWND window, const RGNDATA *dirty) {
	return fakeSwap(nullptr, source, dest, window, dirty, 0);
}
HRESULT WINAPI fakeEx(IDirect3DDevice9Ex *, const RECT *source, const RECT *dest, HWND window, const RGNDATA *dirty, DWORD flags) {
	return fakeSwap(nullptr, source, dest, window, dirty, flags);
}
HRESULT WINAPI fakeReset(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *parameters) { ++testCalls; testParameters = parameters; return testResult; }
HRESULT WINAPI fakeResetEx(IDirect3DDevice9Ex *, D3DPRESENT_PARAMETERS *parameters, D3DDISPLAYMODEEX *mode) {
	++testCalls; testParameters = parameters; testMode = mode; return testResult;
}
}
bool testRenderHooks() {
	static std::array<void *, 137> deviceTable{};
	static std::array<void *, 10> chainTable{};
	static std::array<void *, 10> newChainTable{};
	deviceTable[16] = reinterpret_cast<void *>(&fakeReset); deviceTable[17] = reinterpret_cast<void *>(&fakeDevice);
	deviceTable[121] = reinterpret_cast<void *>(&fakeEx); deviceTable[132] = reinterpret_cast<void *>(&fakeResetEx);
	chainTable[3] = newChainTable[3] = reinterpret_cast<void *>(&fakeSwap);
	auto devicePointer = deviceTable.data(), chainPointer = chainTable.data(), newChainPointer = newChainTable.data();
	auto device = reinterpret_cast<IDirect3DDevice9 *>(&devicePointer);
	auto extended = reinterpret_cast<IDirect3DDevice9Ex *>(&devicePointer);
	auto chain = reinterpret_cast<IDirect3DSwapChain9 *>(&chainPointer);
	auto newChain = reinterpret_cast<IDirect3DSwapChain9 *>(&newChainPointer);
	if (!devicePresent.install(device, onDevicePresent) || !deviceReset.install(device, onReset) ||
		!swapPresent.install(chain, onSwapPresent) || !exPresent.install(extended, onExPresent) || !exReset.install(extended, onResetEx)) return false;
	RECT source{1,2,3,4}, dest{5,6,7,8}; RGNDATA dirty{};
	HWND window = reinterpret_cast<HWND>(uintptr_t{123});
	currentScene.store(4321);
	for (auto result : {S_OK, D3DERR_WASSTILLDRAWING, D3DERR_DEVICELOST, D3DERR_DEVICENOTRESET, D3DERR_INVALIDCALL, E_FAIL}) {
		testResult = result;
		auto before = testCalls;
		auto returned = reinterpret_cast<SwapPresent>(chainTable[3])(chain, &source, &dest, window, &dirty, D3DPRESENT_DONOTWAIT);
		if (returned != result || testCalls != before + 1 || testFlags != D3DPRESENT_DONOTWAIT ||
			testSource != &source || testDest != &dest || testWindow != window || testDirty != &dirty) return false;
	}
	// A newly created swap chain must not lose coverage or call an old target twice.
	if (!swapPresent.install(newChain, onSwapPresent)) return false;
	testResult = S_OK;
	if (reinterpret_cast<SwapPresent>(newChainTable[3])(newChain, nullptr, nullptr, nullptr, nullptr, 0) != S_OK) return false;
	if (reinterpret_cast<DevicePresent>(deviceTable[17])(device, nullptr, nullptr, nullptr, nullptr) != S_OK) return false;
	if (reinterpret_cast<ExPresent>(deviceTable[121])(extended, &source, &dest, window, &dirty, 7) != S_OK || testFlags != 7) return false;
	D3DPRESENT_PARAMETERS parameters{}; D3DDISPLAYMODEEX mode{};
	if (reinterpret_cast<DeviceReset>(deviceTable[16])(device, &parameters) != S_OK || testParameters != &parameters) return false;
	testResult = D3DERR_DEVICELOST;
	if (reinterpret_cast<ExReset>(deviceTable[132])(extended, &parameters, &mode) != testResult || testMode != &mode) return false;
	// Do not clobber a hook installed later by another mod.
	newChainTable[3] = reinterpret_cast<void *>(&fakeSwap);
	if (swapPresent.install(newChain, onSwapPresent) || newChainTable[3] != reinterpret_cast<void *>(&fakeSwap)) return false;
	return true;
}
#endif
}
#endif
