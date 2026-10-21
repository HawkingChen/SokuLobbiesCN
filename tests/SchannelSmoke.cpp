#include <curl/curl.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
static size_t discard(char *, size_t size, size_t count, void *) { return size * count; }
int main() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    const auto *info = curl_version_info(CURLVERSION_NOW);
    std::printf("%s\n", curl_version());
    if (!info->ssl_version || !std::strstr(info->ssl_version, "Schannel")) return 2;
    int failed = 0;
    for (bool post : {false, true}) {
        CURL *curl = curl_easy_init();
        if (!curl) return 3;
        char error[CURL_ERROR_SIZE]{};
        curl_easy_setopt(curl, CURLOPT_URL, post ? "https://soku.cleista.cc/api/v1/ratings/players" : "https://soku.cleista.cc/api/v1/ratings/export.json");
        curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
        curl_slist *headers = nullptr;
        if (post) {
            headers = curl_slist_append(nullptr, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{\"names\":[\"mian\"]}");
        }
        auto result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        std::printf("%s curl=%d HTTP=%ld detail=%s\n", post ? "POST players" : "GET export", int(result), status, error);
        if (result != CURLE_OK || status != 200) failed = 1;
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    }
    curl_global_cleanup();
    return failed;
}
