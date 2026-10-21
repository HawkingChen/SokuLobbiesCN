#include <windows.h>
#include <bcrypt.h>
#include <curl/curl.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <initializer_list>
#undef curl_easy_setopt
#undef curl_easy_getinfo

namespace {
HMODULE selfModule = nullptr, baseModule = nullptr;
std::mutex initMutex, logMutex;
std::filesystem::path logPath;
bool backendReady = false;
const char expectedHash[] = "3f7e979bda4c9e21fcf6b720be6eac6bdc45821163e8e0f31f718d3e7e03cb9a";

void log(const char *text) noexcept {
    try {
        std::lock_guard<std::mutex> lock(logMutex);
        if (logPath.empty()) return;
        std::error_code ec;
        if (std::filesystem::file_size(logPath, ec) > 256 * 1024 && !ec) {
            auto old = logPath; old += L".old";
            std::filesystem::remove(old, ec);
            ec.clear(); std::filesystem::rename(logPath, old, ec);
            if (ec) return;
        }
        std::ofstream f(logPath, std::ios::app);
        f << GetTickCount64() << " " << text << "\n";
    } catch (...) {}
}

bool verified(const wchar_t *path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    DWORD size = 0, written = 0;
    bool ok = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &written, 0) >= 0;
    std::vector<UCHAR> object(size);
    if (ok) ok = BCryptCreateHash(alg, &hash, object.data(), size, nullptr, 0, 0) >= 0;
    char buffer[16384];
    while (ok && file.read(buffer, sizeof(buffer)).gcount()) {
        ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer), static_cast<ULONG>(file.gcount()), 0) >= 0;
    }
    UCHAR digest[32]{};
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    char hex[65]{};
    for (size_t i = 0; i < sizeof(digest); ++i) std::sprintf(hex + i * 2, "%02x", digest[i]);
    return ok && !std::strcmp(hex, expectedHash);
}

CURLcode __cdecl setopt(CURL *easy, CURLoption option, ...) noexcept {
    va_list args;
    va_start(args, option);
    CURLcode result;
    if (option >= CURLOPTTYPE_OFF_T && option < CURLOPTTYPE_BLOB) {
        auto value = va_arg(args, curl_off_t);
        result = curl_easy_setopt(easy, option, value);
    } else if (option >= CURLOPTTYPE_OBJECTPOINT) {
        auto value = va_arg(args, std::uintptr_t);
        // Use system trust even when the original caller supplies its PEM bundle.
        if (option == CURLOPT_CAINFO || option == CURLOPT_CAPATH) value = 0;
        result = curl_easy_setopt(easy, option, value);
    } else {
        auto value = va_arg(args, long);
        result = curl_easy_setopt(easy, option, value);
    }
    va_end(args);
    return result;
}
CURLcode __cdecl perform(CURL *easy) noexcept {
    CURLcode result = curl_easy_perform(easy);
    long http = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http);
    char text[256];
    std::snprintf(text, sizeof(text), "backend=Schannel trust=Windows curl=%d HTTP=%ld %s", int(result), http, curl_easy_strerror(result));
    log(text);
    return result;
}
struct Hook { size_t rva; unsigned char bytes[8]; void *replacement; };

bool prepare(const wchar_t *path, bool test) {
    std::lock_guard<std::mutex> lock(initMutex);
    if (baseModule) return true;
    logPath = std::filesystem::path(path).parent_path() / L"Schannel-transport.log";
    if (!verified(path)) { log("Refused: original DLL does not match verified official 0.7.6.3 SHA256"); return false; }
    if (!backendReady) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return false;
        backendReady = true;
    }
    auto info = curl_version_info(CURLVERSION_NOW);
    if (!info->ssl_version || !std::strstr(info->ssl_version, "Schannel")) return false;
    HMODULE loaded = LoadLibraryExW(path, nullptr, test ? DONT_RESOLVE_DLL_REFERENCES : LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!loaded) { log("Original lobby DLL could not be loaded"); return false; }
    Hook hooks[] = {
        {0x17a9a0, {0x55,0x8b,0xec,0x83,0x7d,0x08,0x00,0x74}, reinterpret_cast<void *>(&curl_easy_cleanup)},
        {0x17a9c0, {0x55,0x8b,0xec,0xff,0x75,0x10,0xff,0x75}, reinterpret_cast<void *>(&curl_easy_getinfo)},
        {0x17a9e0, {0x55,0x8b,0xec,0x51,0x68,0x90,0x4f,0x29}, reinterpret_cast<void *>(&curl_easy_init)},
        {0x17aa40, {0x55,0x8b,0xec,0x51,0x53,0x8b,0x5d,0x08}, reinterpret_cast<void *>(&perform)},
        {0x17e3d0, {0x55,0x8b,0xec,0x8b,0x45,0x08,0x85,0xc0}, reinterpret_cast<void *>(&setopt)}
    };
    auto base = reinterpret_cast<unsigned char *>(loaded);
    // ASLR relocates the immediate global address in curl_easy_init's prologue.
    for (auto &hook : hooks) {
        const size_t checked = hook.rva == 0x17a9e0 ? 5 : 8;
        if (std::memcmp(base + hook.rva, hook.bytes, checked)) { FreeLibrary(loaded); log("Refused: API signature mismatch"); return false; }
    }
    DWORD protect[5]{};
    size_t ready = 0;
    for (; ready < 5; ++ready) {
        if (!VirtualProtect(base + hooks[ready].rva, 5, PAGE_EXECUTE_READWRITE, &protect[ready])) break;
    }
    if (ready != 5) {
        for (size_t i = ready; i-- > 0;) { DWORD ignored; VirtualProtect(base + hooks[i].rva, 5, protect[i], &ignored); }
        FreeLibrary(loaded); return false;
    }
    for (size_t i = 0; i < 5; ++i) {
        auto target = base + hooks[i].rva;
        auto delta = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(hooks[i].replacement) - reinterpret_cast<std::uintptr_t>(target + 5));
        target[0] = 0xe9; std::memcpy(target + 1, &delta, 4);
        FlushInstructionCache(GetCurrentProcess(), target, 5);
    }
    // Several API entries share one page. Keep all pages writable until every
    // jump is installed, then unwind protections in reverse acquisition order.
    for (size_t i = 5; i-- > 0;) {
        DWORD ignored; VirtualProtect(base + hooks[i].rva, 5, protect[i], &ignored);
    }
    baseModule = loaded;
    log("Verified official 0.7.6.3 loaded unchanged; five libcurl API entries redirected in memory; protocol preserved");
    log(curl_version());
    return true;
}
std::filesystem::path originalPath() {
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(selfModule, path, 32768);
    if (!n || n >= 32768) return {};
    return std::filesystem::path(path).parent_path() / L"SokuLobbiesMod.dll";
}
size_t discard(char *, size_t s, size_t n, void *) { return s * n; }
}

extern "C" __declspec(dllexport) bool CheckVersion(const BYTE hash[16]) noexcept {
    try {
        auto path = originalPath(); if (!prepare(path.c_str(), false)) return false;
        auto f = reinterpret_cast<bool (__cdecl *)(const BYTE *)>(GetProcAddress(baseModule, "CheckVersion"));
        return f && f(hash);
    } catch (...) { return false; }
}
extern "C" __declspec(dllexport) bool Initialize(HMODULE, HMODULE parent) noexcept {
    try {
        auto path = originalPath(); if (!prepare(path.c_str(), false)) return false;
        auto f = reinterpret_cast<bool (__cdecl *)(HMODULE, HMODULE)>(GetProcAddress(baseModule, "Initialize"));
        return f && f(baseModule, parent);
    } catch (...) { return false; }
}
extern "C" __declspec(dllexport) int getPriority() { return -50000; }
extern "C" __declspec(dllexport) int TestTransport(const wchar_t *path) {
    std::fprintf(stderr, "prepare original DLL\n");
    if (!prepare(path, true)) { std::puts("prepare failed"); return 10; }
    auto base = reinterpret_cast<unsigned char *>(baseModule);
    auto init = reinterpret_cast<CURL *(__cdecl *)()>(base + 0x17a9e0);
    auto set = reinterpret_cast<CURLcode (__cdecl *)(CURL *, CURLoption, ...)>(base + 0x17e3d0);
    auto run = reinterpret_cast<CURLcode (__cdecl *)(CURL *)>(base + 0x17aa40);
    auto get = reinterpret_cast<CURLcode (__cdecl *)(CURL *, CURLINFO, ...)>(base + 0x17a9c0);
    auto clean = reinterpret_cast<void (__cdecl *)(CURL *)>(base + 0x17a9a0);
    std::fprintf(stderr, "%s\n", curl_version());
    int failed = 0;
    for (bool post : {false, true}) {
        std::fprintf(stderr, "create request post=%d\n", int(post));
        CURL *easy = init(); if (!easy) return 11;
        set(easy, CURLOPT_URL, post ? "https://soku.cleista.cc/api/v1/ratings/players" : "https://soku.cleista.cc/api/v1/ratings/export.json");
        set(easy, CURLOPT_CAINFO, "Z:/deliberately-nonexistent-ca-bundle.crt");
        set(easy, CURLOPT_SSL_VERIFYPEER, 1L); set(easy, CURLOPT_SSL_VERIFYHOST, 2L);
        set(easy, CURLOPT_CONNECTTIMEOUT, 10L); set(easy, CURLOPT_TIMEOUT, 30L);
        set(easy, CURLOPT_WRITEFUNCTION, &discard);
        char error[CURL_ERROR_SIZE]{}; set(easy, CURLOPT_ERRORBUFFER, error);
        curl_slist *headers = nullptr;
        if (post) {
            headers = curl_slist_append(nullptr, "Content-Type: application/json");
            set(easy, CURLOPT_HTTPHEADER, headers);
            set(easy, CURLOPT_POSTFIELDS, "{\"names\":[\"mian\"]}");
            set(easy, CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(18));
        }
        std::fprintf(stderr, "perform request\n");
        auto result = run(easy); long status = 0; get(easy, CURLINFO_RESPONSE_CODE, &status);
        std::printf("official API detour %s curl=%d HTTP=%ld detail=%s\n", post ? "POST" : "GET", int(result), status, error);
        if (result || status != 200) failed = 1;
        clean(easy); curl_slist_free_all(headers);
    }
    return failed;
}
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) selfModule = module;
    return TRUE;
}
