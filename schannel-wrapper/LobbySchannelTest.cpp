#include <windows.h>
#include <cstdio>
int wmain(int argc, wchar_t **argv) {
    if (argc != 2) return 2;
    HMODULE module = LoadLibraryW(L"LobbySchannel.dll");
    if (!module) { std::printf("wrapper load failed: %lu\n", GetLastError()); return 3; }
    auto test = reinterpret_cast<int (__cdecl *)(const wchar_t *)>(GetProcAddress(module, "TestTransport"));
    std::fprintf(stderr, "wrapper loaded; export=%p\n", reinterpret_cast<void *>(test));
    __try { return test ? test(argv[1]) : 4; }
    __except(EXCEPTION_EXECUTE_HANDLER) { std::fprintf(stderr, "SEH exception=0x%08lx\n", GetExceptionCode()); return 20; }
}
