#include "RatingsNetworkDiagnostics.hpp"

#ifdef SOKU_RATINGS_NETWORK_PROBE
#include <Windows.h>
#include <winhttp.h>
#include <curl/curl.h>
#include <chrono>
#include <algorithm>

namespace {
	constexpr size_t maxBodySize = 12u * 1024u * 1024u;
	struct HttpHandle {
		HINTERNET value;
		explicit HttpHandle(HINTERNET value) : value(value) {}
		~HttpHandle() { if (value) WinHttpCloseHandle(value); }
		HttpHandle(const HttpHandle &) = delete;
		HttpHandle &operator=(const HttpHandle &) = delete;
	};
	struct ProbeContext {
		std::string body;
		const std::atomic_bool &enabled;
		const std::atomic_bool &stopping;
	};
	size_t receive(char *data, size_t size, size_t count, void *opaque)
	{
		auto &ctx = *static_cast<ProbeContext *>(opaque);
		if (size && count > maxBodySize / size) return 0;
		const auto bytes = size * count;
		if (bytes > maxBodySize - ctx.body.size()) return 0;
		ctx.body.append(data, bytes);
		return bytes;
	}
	int progress(void *opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
	{
		const auto &ctx = *static_cast<ProbeContext *>(opaque);
		return !ctx.enabled.load() || ctx.stopping.load();
	}

	void probeWindows(const std::wstring &path, const std::string *postBody, DWORD access,
		const char *mode, const std::atomic_bool &enabled, const std::atomic_bool &stopping,
		const std::function<void(const std::string &)> &log)
	{
		const auto started = std::chrono::steady_clock::now();
		DWORD error = 0, status = 0;
		size_t bytes = 0;
		std::string stage = "open";
		log(std::string("probe start backend=WinHTTP/Schannel mode=") + mode + " verify=Windows-trust-store TLS=1.2+ redirects=disabled");
		auto run = [&] {
			HttpHandle session(WinHttpOpen(L"SokuLobbies-RelativeRatings/1.0", access,
				WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
			if (!session.value) { error = GetLastError(); return; }
			if (!WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000)) { error = GetLastError(); return; }
			DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
			tls |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
			if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof(tls))) {
				tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
				if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof(tls))) { error = GetLastError(); return; }
			}
			HttpHandle connection(WinHttpConnect(session.value, L"soku.cleista.cc", INTERNET_DEFAULT_HTTPS_PORT, 0));
			if (!connection.value) { error = GetLastError(); return; }
			HttpHandle request(WinHttpOpenRequest(connection.value, postBody ? L"POST" : L"GET", path.c_str(),
				nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
			if (!request.value) { error = GetLastError(); return; }
			DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
			if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects))) { error = GetLastError(); return; }
			auto cancelled = [&] {
				if (!enabled.load() || stopping.load()) { error = ERROR_CANCELLED; return true; }
				if (std::chrono::steady_clock::now() - started > std::chrono::seconds(15)) { error = ERROR_WINHTTP_TIMEOUT; return true; }
				return false;
			};
			stage = "send/TLS";
			if (cancelled()) return;
			const auto length = postBody ? static_cast<DWORD>(postBody->size()) : 0;
			if (!WinHttpSendRequest(request.value, postBody ? L"Content-Type: application/json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS,
				postBody ? static_cast<DWORD>(-1) : 0, postBody ? const_cast<char *>(postBody->data()) : WINHTTP_NO_REQUEST_DATA,
				length, length, 0)) { error = GetLastError(); return; }
			stage = "response_headers";
			if (cancelled()) return;
			if (!WinHttpReceiveResponse(request.value, nullptr)) { error = GetLastError(); return; }
			DWORD statusSize = sizeof(status);
			if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) { error = GetLastError(); return; }
			stage = "response_body";
			for (;;) {
				if (cancelled()) return;
				char buffer[8192];
				DWORD read = 0;
				if (!WinHttpReadData(request.value, buffer, sizeof(buffer), &read)) { error = GetLastError(); return; }
				if (!read) break;
				if (read > maxBodySize - bytes) { error = ERROR_BUFFER_OVERFLOW; return; }
				bytes += read;
			}
			stage = "complete";
		};
		run();
		log(std::string("probe end backend=WinHTTP/Schannel mode=") + mode + " stage=" + stage +
			" winhttp_error=" + std::to_string(error) + " http=" + std::to_string(status) + " bytes=" + std::to_string(bytes) +
			" total_ms=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()));
	}
}

void logRatingsNetworkEnvironment(const std::function<void(const std::string &)> &log)
{
	log(std::string("network diagnostics v4; build=Release-probe; ") + curl_version() + "; probe_results_do_not_replace_ratings");
	WINHTTP_CURRENT_USER_IE_PROXY_CONFIG user{};
	if (WinHttpGetIEProxyConfigForCurrentUser(&user)) {
		log(std::string("Windows user proxy: manual=") + (user.lpszProxy && *user.lpszProxy ? "yes" : "no") +
			" PAC=" + (user.lpszAutoConfigUrl && *user.lpszAutoConfigUrl ? "yes" : "no") + " autodetect=" + (user.fAutoDetect ? "yes" : "no"));
		GlobalFree(user.lpszProxy); GlobalFree(user.lpszProxyBypass); GlobalFree(user.lpszAutoConfigUrl);
	} else log("Windows user proxy query error=" + std::to_string(GetLastError()));
	for (const auto name : {"https_proxy", "HTTPS_PROXY", "ALL_PROXY", "all_proxy", "NO_PROXY", "no_proxy"}) {
		SetLastError(ERROR_SUCCESS);
		const auto length = GetEnvironmentVariableA(name, nullptr, 0);
		log(std::string("proxy environment ") + name + "=" + (length ? "set (value omitted)" : "unset"));
	}
}

void probeRatingsNetwork(const std::string &url, const std::string *postBody, const std::string &caPath,
	const std::atomic_bool &enabled, const std::atomic_bool &stopping,
	const std::function<void(const std::string &)> &log)
{
	const std::string base = "https://soku.cleista.cc/";
	if (url.compare(0, base.size(), base) != 0) return;
	log("network comparison BEGIN (once per game session); same API/method/body; certificate verification enabled; bodies/credentials omitted");
	for (const long family : {CURL_IPRESOLVE_V4, CURL_IPRESOLVE_V6}) {
		if (!enabled.load() || stopping.load()) break;
		CURL *curl = curl_easy_init();
		if (!curl) break;
		ProbeContext ctx{{}, enabled, stopping};
		char error[CURL_ERROR_SIZE]{};
		curl_slist *headers = postBody ? curl_slist_append(nullptr, "Content-Type: application/json") : nullptr;
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_PROXY, ""); // Compare direct routes, independently of proxy environment.
		curl_easy_setopt(curl, CURLOPT_IPRESOLVE, family);
		curl_easy_setopt(curl, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2));
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
		curl_easy_setopt(curl, CURLOPT_CAINFO, caPath.c_str());
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "SokuLobbies-RelativeRatings/1.0");
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
		if (postBody) {
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody->c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(postBody->size()));
		}
		log(std::string("probe start backend=mbedTLS direct=true TLS=1.2 family=") + (family == CURL_IPRESOLVE_V4 ? "IPv4" : "IPv6"));
		const auto result = curl_easy_perform(curl);
		long http = 0;
		char *ip = nullptr;
		double total = 0, tcp = 0, tls = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
		curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip);
		curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);
		curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &tcp);
		curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &tls);
		log(std::string("probe end backend=mbedTLS family=") + (family == CURL_IPRESOLVE_V4 ? "IPv4" : "IPv6") +
			" remote=[" + (ip ? ip : "unknown") + "] curl_code=" + std::to_string(result) + " http=" + std::to_string(http) +
			" bytes=" + std::to_string(ctx.body.size()) + " cumulative_ms[tcp,tls,total]=" +
			std::to_string(tcp * 1000) + "," + std::to_string(tls * 1000) + "," + std::to_string(total * 1000) + " detail=" + error);
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
	}
	const std::wstring path(url.begin() + base.size() - 1, url.end());
	if (enabled.load() && !stopping.load())
		probeWindows(path, postBody, WINHTTP_ACCESS_TYPE_NO_PROXY, "direct", enabled, stopping, log);
	if (enabled.load() && !stopping.load())
		probeWindows(path, postBody, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, "Windows-auto-proxy", enabled, stopping, log);
	log("network comparison END; normal rating queries still use original libcurl/mbedTLS path");
}
#endif
