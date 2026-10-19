#pragma once

struct IDirect3DDevice9;
namespace ClientRenderDiagnostics {
#ifdef SOKU_CLIENT_PERFORMANCE
void sample(IDirect3DDevice9 *device, unsigned scene) noexcept;
#else
inline void sample(IDirect3DDevice9 *, unsigned) noexcept {}
#endif
}
