#ifndef SOKULOBBIES_IDLE_TRACKING_HPP
#define SOKULOBBIES_IDLE_TRACKING_HPP

#include <chrono>
#include <algorithm>
#include <string_view>
#include <cstdint>

namespace IdleTracking {
using Clock = std::chrono::steady_clock;
constexpr auto REPORT_INTERVAL = std::chrono::seconds(10);
constexpr auto REPORT_LIFETIME = std::chrono::seconds(30);
constexpr uint64_t MAX_IDLE_SECONDS = 365ULL * 24 * 60 * 60;

inline bool isControl(std::string_view message)
{
	return message == "/__idlecap" || message == "/__idle" || message.substr(0, 8) == "/__idle ";
}

inline bool parseSeconds(std::string_view text, uint64_t &seconds)
{
	if (text.empty() || text.size() > 8) return false;
	uint64_t value = 0;
	for (char c : text) {
		if (c < '0' || c > '9') return false;
		value = value * 10 + (c - '0');
		if (value > MAX_IDLE_SECONDS) return false;
	}
	seconds = value;
	return true;
}

// Caller serializes access. Movement and network heartbeats are not activity.
struct LocalClock {
	Clock::time_point lastActivity{}, lastReport{};
	bool exempt = false, supported = false, dirty = true;
	void reset(Clock::time_point now) { *this = {}; lastActivity = now; }
	void active(Clock::time_point now) { lastActivity = now; dirty = true; }
	void setExempt(bool value, Clock::time_point now) {
		if (value == exempt) return;
		exempt = value; active(now);
	}
	std::chrono::seconds idle(Clock::time_point now) const {
		return exempt ? std::chrono::seconds(0) : std::chrono::duration_cast<std::chrono::seconds>(now - lastActivity);
	}
	bool due(Clock::time_point now) const {
		// Coalesce rapid events to at most one report per second.
		return supported && (lastReport == Clock::time_point{} ||
			(now - lastReport >= std::chrono::seconds(1) && (dirty || now - lastReport >= REPORT_INTERVAL)));
	}
};

struct RemoteClock {
	Clock::time_point received{};
	std::chrono::seconds reported{0};
	bool present = false, exempt = false;
	void reset() { *this = {}; }
	void accept(uint64_t seconds, bool exemption, Clock::time_point now) {
		received = now; reported = std::chrono::seconds(seconds); exempt = exemption; present = true;
	}
	bool fresh(Clock::time_point now) const { return present && now >= received && now - received < REPORT_LIFETIME; }
	std::chrono::seconds idle(Clock::time_point now, Clock::time_point epoch) const {
		if (exempt) return std::chrono::seconds(0);
		const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - received);
		const auto sessionAge = std::chrono::duration_cast<std::chrono::seconds>((std::max)(Clock::duration::zero(), now - epoch));
		return (std::min)(reported + elapsed, sessionAge);
	}
};
}
#endif
