//
// Created by PinkySmile on 04/11/2020.
//

#include "getPublicIp.hpp"
#include <windows.h>
#include <cstdio>
#include <mutex>
#include <string>
#include <chrono>
#include <stdexcept>
#include "data.hpp"
#include "LobbyData.hpp"

static std::mutex myIpv6Mutex;
static std::string myIpv6("");
static std::timed_mutex myIpMutex;
static std::string myIp;

std::string getMyIp(const std::atomic_bool *cancel)
{
	auto checkCancelled = [&] {
		if (cancel && !cancel->load()) throw std::runtime_error("IP lookup cancelled");
	};
	checkCancelled();
	wchar_t buffer2[256]{};
	char buffer[256]{};
	auto bufferSize = sizeof(buffer2)/sizeof(*buffer2);
	GetPrivateProfileStringW(L"Lobby", L"HostIP", L"", buffer2, bufferSize, profilePath);
	if (*buffer2) {
		if (!WideCharToMultiByte(CP_UTF8, 0, buffer2, -1, buffer, sizeof(buffer), nullptr, nullptr))
			throw std::runtime_error("Invalid HostIP configuration");
		printf("Using forced ip %s\n", buffer);
		return buffer;
	}
	// Serialize cache misses, but allow a departing caller to cancel while waiting.
	std::unique_lock<std::timed_mutex> lock(myIpMutex, std::defer_lock);
	while (!lock.try_lock_for(std::chrono::milliseconds(50))) checkCancelled();
	checkCancelled();
	if (!myIp.empty())
		return myIp;

	puts("Fetching public IP");
	try {
		GetPrivateProfileStringW(L"Lobby", L"GetPublicIpServer", L"http://www.sfml-dev.org/ip-provider.php", buffer2, bufferSize, profilePath);
		// https://curl.se/libcurl/c/CURLOPT_URL.html
		if (!WideCharToMultiByte(CP_UTF8, 0, buffer2, -1, buffer, sizeof(buffer), nullptr, nullptr))
			throw std::runtime_error("Invalid public IP service configuration");

		auto ip = lobbyData->httpRequest(buffer, "GET", "", 16000, cancel);

		ip.erase(0, ip.find_first_not_of(" \n\r\t"));
		ip.erase(ip.find_last_not_of(" \n\r\t") + 1);
		checkCancelled();
		if (ip.empty()) throw std::runtime_error("Public IP service returned an empty address");
		myIp = std::move(ip);
		printf("My ip is %s\n", myIp.c_str());
		return myIp;
	} catch (std::exception &e) {
		printf("Error: %s\n", e.what());
		throw;
	}
}

std::string getMyIpv6()
{
	std::lock_guard<std::mutex> lock(myIpv6Mutex);
	std::string _myIpv6 = myIpv6;
	return _myIpv6;
}

bool isIpv6Available() {
	std::lock_guard<std::mutex> lock(myIpv6Mutex);
	bool ret = !myIpv6.empty();
	return ret;
}

void setMyIpv6(std::string &ipv6) {
	std::lock_guard<std::mutex> lock(myIpv6Mutex);
	myIpv6 = ipv6;
}
