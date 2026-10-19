#pragma once

#include <chrono>
#include <cstdint>

namespace ClientPerformance {
enum class Stage { UpdateHook, OriginalUpdate, EndScene, LobbyUpdate, LobbyRender,
	NetworkPump, TextureWork, ChatRender, NameRender, BubbleRender, RatingCards, RatingLookup, RatingBatch, TextTexture, Snapshot,
	Present, TextCreate, TextGetDC, TextGDI, TextLock, TextPixels, TextUpload, RenderGap,
	SwapPresent, PresentEx, Reset, ResetEx, DeviceSample, Count };
enum class Gauge { Players, ChatMessages, ChatFragments, NameTextures, TextBubbles, EmoteBubbles,
	RatingCards, PendingNames, PendingBubbles, PendingRemovals, InboxFrames, InboxBytes,
	SendBytes, Connections, RatingPending, RatingCache, PendingNameQueue, PendingBubbleQueue, RatingTextures,
	PresentHook, DeviceState, SoftwareVP, DeviceFlags, ViewportWidth, ViewportHeight, TextD3D9Ex,
	SwapPresentHook, ResetHook, PresentExHook, ResetExHook, DeviceEpoch, SwapEpoch,
	SwapEffect, BackBuffers, PresentationInterval, Windowed, Count };

enum class RenderCall { DevicePresent, SwapPresent, PresentEx, Reset, ResetEx, Count };

#ifdef SOKU_CLIENT_PERFORMANCE
void record(Stage stage, uint64_t microseconds) noexcept;
void gauge(Gauge which, uint64_t value) noexcept;
void texture(bool succeeded) noexcept;
void tick(const wchar_t *folder, unsigned scene, bool lobby, const char *version) noexcept;
bool snapshotDue(uint64_t &last) noexcept;
void renderBegin(RenderCall call, unsigned scene) noexcept;
void renderEnd(RenderCall call, int32_t result, uint32_t flags, unsigned scene) noexcept;
class Scope {
	Stage _stage;
	std::chrono::steady_clock::time_point _start = std::chrono::steady_clock::now();
public:
	explicit Scope(Stage stage) noexcept : _stage(stage) {}
	void next(Stage stage) noexcept {
		auto now = std::chrono::steady_clock::now();
		record(_stage, std::chrono::duration_cast<std::chrono::microseconds>(now - _start).count());
		_stage = stage; _start = now;
	}
	~Scope() noexcept { record(_stage, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _start).count()); }
};
#else
inline void gauge(Gauge, uint64_t) noexcept {}
inline void texture(bool) noexcept {}
inline void tick(const wchar_t *, unsigned, bool, const char *) noexcept {}
inline bool snapshotDue(uint64_t &) noexcept { return false; }
class Scope { public: explicit Scope(Stage) noexcept {} void next(Stage) noexcept {} };
#endif
}
