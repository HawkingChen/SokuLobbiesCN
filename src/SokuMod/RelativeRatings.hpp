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
	std::wstring _moduleDirectory;
	std::wstring _cachePath;
	std::wstring _mapPath;
	std::wstring _caPath;
	std::wstring _debugLogPath;
	mutable std::mutex _mutex;
	mutable std::condition_variable _wake;
	std::thread _worker;
	std::atomic_bool _enabled{false};
	std::atomic_bool _stopping{false};
	std::unordered_map<std::string, Rating> _byPlayedAs;
	std::unordered_map<std::string, Rating> _byName;
	std::unordered_map<std::string, Rating> _byHandle;
	std::unordered_map<std::string, Rating> _byQuery;
	std::unordered_map<std::string, std::string> _manualAccounts;
	std::unordered_map<std::string, unsigned> _nameCounts;
	mutable std::vector<std::string> _pendingProfiles;
	mutable std::unordered_map<std::string, std::uint64_t> _profileChecked;
	std::unordered_map<std::string, std::string> _profileHandles;
	std::string _etag;
	std::uint64_t _updatedAt = 0;
	bool _refreshRequested = true;
	std::string _status = "Not loaded";

	void _run();
	void _debugLog(const std::string &message) const;
	bool _loadCache();
	void _loadMappings();
	bool _download();
	void _downloadProfile(const std::string &profile);
	void _downloadProfiles(const std::vector<std::string> &profiles);
	bool _publish(const std::string &json, std::uint64_t updatedAt);
	void _writeCache(const std::string &json, std::uint64_t updatedAt);

public:
	explicit RelativeRatings(const std::wstring &moduleDirectory);
	~RelativeRatings();
	RelativeRatings(const RelativeRatings &) = delete;
	RelativeRatings &operator=(const RelativeRatings &) = delete;

	void setEnabled(bool enabled);
	void stop();
	void refresh();
	bool setMapping(const std::string &profile, const std::string &account);
	bool lookup(const std::string &profile, Rating &rating) const;
	bool hasData() const;
	std::string status() const;
};
