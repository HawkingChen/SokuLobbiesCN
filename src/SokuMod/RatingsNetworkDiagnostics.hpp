#pragma once

#ifdef SOKU_RATINGS_NETWORK_PROBE
#include <atomic>
#include <functional>
#include <string>

void logRatingsNetworkEnvironment(const std::function<void(const std::string &)> &log);
void probeRatingsNetwork(const std::string &url, const std::string *postBody, const std::string &caPath,
	const std::atomic_bool &enabled, const std::atomic_bool &stopping,
	const std::function<void(const std::string &)> &log);
#endif
