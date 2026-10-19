#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class RelativeRatings {
public:
	struct CharacterRating {
		std::string code;
		double rating = 0;
		unsigned games = 0;
	};
	struct Rating {
		std::string name;
		std::string playedAs;
		std::string handle;
		double value = 0;
		double deviation = 0;
		unsigned games = 0;
		std::vector<CharacterRating> characters;
	};

private:
	std::wstring _cachePath;
	std::wstring _mapPath;
	std::wstring _caPath;
	std::wstring _debugLogPath;
	mutable std::mutex _mutex;
	mutable std::condition_variable _wake;
	std::thread _worker;
	std::atomic_bool _enabled{false};
	std::atomic_bool _stopping{false};
	std::atomic_bool _workerFailed{false};
	mutable std::atomic_ullong _lastInvalidNameLogAt{0};
	std::unordered_map<std::string, Rating> _byPlayedAs;
	std::unordered_map<std::string, Rating> _byName;
	std::unordered_map<std::string, Rating> _byHandle;
	std::unordered_map<std::string, Rating> _byQuery;
	std::unordered_map<std::string, std::string> _manualAccounts;
	std::unordered_map<std::string, unsigned> _nameCounts;
	mutable std::vector<std::string> _pendingProfiles;
	mutable std::unordered_map<std::string, std::uint64_t> _profileChecked;
	std::unordered_map<std::string, std::string> _profileHandles;
#ifdef SOKU_RATINGS_NETWORK_PROBE
	bool _networkProbeDone = false; // Worker thread only.
#endif

	void _run();
	void _runGuarded() noexcept;
	void _debugLog(const std::string &message) const;
	void _loadMappings();
	void _downloadProfiles(const std::vector<std::string> &profiles);
	bool _publish(const std::string &json);

public:
	explicit RelativeRatings(const std::wstring &moduleDirectory);
	~RelativeRatings();
	RelativeRatings(const RelativeRatings &) = delete;
	RelativeRatings &operator=(const RelativeRatings &) = delete;

	void setEnabled(bool enabled);
	void stop();
	bool setMapping(const std::string &profile, const std::string &account);
	bool lookup(const std::string &profile, Rating &rating) const;
	bool hasData() const;
};
