#include "RelativeRatings.hpp"

#include <Windows.h>
#include <curl/curl.h>
#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace {
	using Json = nlohmann::json;
	constexpr std::size_t maxResponseSize = 12u * 1024u * 1024u;
	std::uint64_t fileTimeNow()
	{
		FILETIME value{};
		GetSystemTimeAsFileTime(&value);
		return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
	}

	std::string trim(std::string value)
	{
		auto first = value.find_first_not_of(" \t\r\n");
		if (first == std::string::npos)
			return {};
		auto last = value.find_last_not_of(" \t\r\n");
		return value.substr(first, last - first + 1);
	}

	struct DownloadBuffer {
		std::string body;
		std::atomic_bool *enabled = nullptr;
		std::atomic_bool *stopping = nullptr;
	};

	size_t receiveBody(char *data, size_t size, size_t count, void *context)
	{
		auto *buffer = static_cast<DownloadBuffer *>(context);
		if (size && count > maxResponseSize / size)
			return 0;
		size_t bytes = size * count;
		if (bytes > maxResponseSize - buffer->body.size())
			return 0;
		buffer->body.append(data, bytes);
		return bytes;
	}

	int transferProgress(void *context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
	{
		auto *buffer = static_cast<DownloadBuffer *>(context);
		return !buffer->enabled->load(std::memory_order_relaxed) || buffer->stopping->load(std::memory_order_relaxed);
	}

	bool validRecord(const RelativeRatings::Rating &record)
	{
		return !record.name.empty() && std::isfinite(record.value) && record.value > 0 &&
			std::isfinite(record.deviation) && record.deviation >= 0;
	}

	std::string optionalString(const Json &object, const char *key)
	{
		auto item = object.find(key);
		return item != object.end() && item->is_string() ? item->get<std::string>() : std::string{};
	}

	std::uint64_t cacheTime(const std::filesystem::path &path)
	{
		WIN32_FILE_ATTRIBUTE_DATA attributes{};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
			return 0;
		return (static_cast<std::uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
			attributes.ftLastWriteTime.dwLowDateTime;
	}
}

RelativeRatings::RelativeRatings(const std::wstring &moduleDirectory) :
	_cachePath((std::filesystem::path(moduleDirectory) / L"ratings-cache").wstring()),
	_mapPath((std::filesystem::path(moduleDirectory) / L"ratings-map.tsv").wstring()),
	_caPath((std::filesystem::path(moduleDirectory) / L"assets/ratings/curl-ca-bundle.crt").wstring()),
	_debugLogPath((std::filesystem::path(moduleDirectory) / L"ratings-debug.log").wstring())
{
}

void RelativeRatings::_debugLog(const std::string &message) const
{
#ifdef SOKU_RATINGS_DIAGNOSTICS
	static std::mutex logMutex;
	std::lock_guard<std::mutex> lock(logMutex);
	try {
		SYSTEMTIME now{};
		GetLocalTime(&now);
		std::ofstream file(_debugLogPath, std::ios::binary | std::ios::app);
		if (file)
			file << std::setfill('0') << std::setw(2) << now.wHour << ':' << std::setw(2) << now.wMinute << ':' <<
				std::setw(2) << now.wSecond << '.' << std::setw(3) << now.wMilliseconds << ' ' << message << "\r\n";
	} catch (...) {
	}
#else
	(void)message;
#endif
}

RelativeRatings::~RelativeRatings()
{
	stop();
}

void RelativeRatings::setEnabled(bool enabled)
{
	if (enabled && !_worker.joinable()) {
		_stopping.store(false, std::memory_order_relaxed);
		_worker = std::thread(&RelativeRatings::_run, this);
	}
	_enabled.store(enabled, std::memory_order_relaxed);
	_wake.notify_all();
	_debugLog(std::string("ratings enabled=") + (enabled ? "true" : "false"));
}

void RelativeRatings::stop()
{
	_stopping.store(true, std::memory_order_relaxed);
	_enabled.store(false, std::memory_order_relaxed);
	_wake.notify_all();
	if (_worker.joinable())
		_worker.join();
}

bool RelativeRatings::setMapping(const std::string &profile, const std::string &account)
{
	const auto cleanProfile = trim(profile);
	const auto cleanAccount = trim(account);
	if (cleanProfile.empty() || cleanProfile.size() >= 129 || cleanAccount.size() >= 129 ||
		cleanProfile.find_first_of("\t\r\n") != std::string::npos || cleanAccount.find_first_of("\t\r\n") != std::string::npos)
		return false;
	std::vector<std::string> lines;
	{
		std::ifstream file(_mapPath, std::ios::binary);
		std::string line;
		while (std::getline(file, line)) {
			auto tab = line.find('\t');
			if (tab != std::string::npos && trim(line.substr(0, tab)) == cleanProfile)
				continue;
			lines.push_back(std::move(line));
		}
	}
	if (!cleanAccount.empty())
		lines.push_back(cleanProfile + "\t" + cleanAccount);
	auto temporary = _mapPath + L".tmp";
	try {
		{
			std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
			for (const auto &line : lines)
				file << line << "\r\n";
			if (!file)
				return false;
		}
		if (!MoveFileExW(temporary.c_str(), _mapPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			DeleteFileW(temporary.c_str());
			return false;
		}
	} catch (...) {
		DeleteFileW(temporary.c_str());
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (cleanAccount.empty())
			_manualAccounts.erase(cleanProfile);
		else
			_manualAccounts[cleanProfile] = cleanAccount;
	}
	return true;
}

bool RelativeRatings::lookup(const std::string &profile, Rating &rating) const
{
	const auto name = trim(profile);
	if (name.empty())
		return false;
	std::lock_guard<std::mutex> lock(_mutex);
	if (_enabled.load(std::memory_order_relaxed)) {
		const auto now = fileTimeNow();
		auto checked = _profileChecked.find(name);
		if ((checked == _profileChecked.end() || now < checked->second || now - checked->second > 6000000000ULL) && _pendingProfiles.size() < 1000) {
			_profileChecked[name] = now;
			_pendingProfiles.push_back(name);
			_wake.notify_all();
		}
	}
	auto mapping = _manualAccounts.find(name);
	if (mapping != _manualAccounts.end()) {
		auto byHandle = _byHandle.find(mapping->second);
		if (byHandle != _byHandle.end()) {
			rating = byHandle->second;
			return true;
		}
		auto byName = _byName.find(mapping->second);
		if (byName != _byName.end() && _nameCounts.at(mapping->second) == 1) {
			rating = byName->second;
			return true;
		}
	}
	auto byQuery = _byQuery.find(name);
	if (byQuery != _byQuery.end()) {
		rating = byQuery->second;
		return true;
	}
	auto claimed = _profileHandles.find(name);
	if (claimed != _profileHandles.end()) {
		auto account = _byHandle.find(claimed->second);
		if (account != _byHandle.end()) {
			rating = account->second;
			return true;
		}
	}
	auto playedAs = _byPlayedAs.find(name);
	if (playedAs != _byPlayedAs.end()) {
		rating = playedAs->second;
		return true;
	}
	auto byName = _byName.find(name);
	if (byName == _byName.end() || _nameCounts.at(name) != 1)
		return false;
	rating = byName->second;
	return true;
}

bool RelativeRatings::hasData() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return !_byName.empty();
}

void RelativeRatings::_loadMappings()
{
	std::unordered_map<std::string, std::string> mappings;
	std::ifstream file(_mapPath, std::ios::binary);
	std::string line;
	while (std::getline(file, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		auto tab = line.find('\t');
		if (tab == std::string::npos)
			continue;
		auto profile = trim(line.substr(0, tab));
		auto account = trim(line.substr(tab + 1));
		if (!profile.empty() && !account.empty() && profile.size() < 129 && account.size() < 129)
			mappings.emplace(std::move(profile), std::move(account));
	}
	std::lock_guard<std::mutex> lock(_mutex);
	_manualAccounts.swap(mappings);
}

bool RelativeRatings::_publish(const std::string &text)
{
	try {
		Json root = Json::parse(text);
		if (!root.is_object() || !root.contains("players") || !root["players"].is_array())
			return false;
		std::unordered_map<std::string, Rating> playedAs;
		std::unordered_map<std::string, Rating> byName;
		std::unordered_map<std::string, Rating> byHandle;
		std::unordered_map<std::string, unsigned> nameCounts;
		std::size_t total = 0;
		for (const auto &entry : root["players"]) {
			if (!entry.is_object())
				return false;
			Rating record;
			record.name = optionalString(entry, "name");
			record.playedAs = optionalString(entry, "playedAs");
			record.handle = optionalString(entry, "handle");
			record.value = entry.value("rating", 0.0);
			record.deviation = entry.value("rd", 0.0);
			record.games = entry.value("games", 0u);
			if (!validRecord(record))
				return false;
			if (entry.contains("characters") && entry["characters"].is_array()) {
				for (const auto &character : entry["characters"]) {
					if (!character.is_object())
						continue;
					CharacterRating item;
					item.code = character.value("character", std::string{});
					item.rating = character.value("rating", 0.0);
					item.games = character.value("games", 0u);
					if (item.code.empty() || !std::isfinite(item.rating) || item.rating <= 0)
						continue;
					record.characters.push_back(std::move(item));
				}
				std::stable_sort(record.characters.begin(), record.characters.end(), [](const auto &left, const auto &right) {
					return left.games == right.games ? left.rating > right.rating : left.games > right.games;
				});
				if (record.characters.size() > 4)
					record.characters.resize(4);
			}
			++nameCounts[record.name];
			if (!record.playedAs.empty())
				playedAs.emplace(record.playedAs, record);
			if (!record.handle.empty())
				byHandle.emplace(record.handle, record);
			byName.emplace(record.name, std::move(record));
			if (++total > 12000)
				return false;
		}
		if (total == 0)
			return false;
		std::lock_guard<std::mutex> lock(_mutex);
		for (auto &entry : playedAs)
			_byPlayedAs.insert_or_assign(entry.first, std::move(entry.second));
		for (auto &entry : byHandle)
			_byHandle.insert_or_assign(entry.first, std::move(entry.second));
		for (auto &entry : byName) {
			_nameCounts[entry.first] = 1;
			_byName.insert_or_assign(entry.first, std::move(entry.second));
		}
		for (const auto &entry : root["players"])
			if (entry.is_object()) {
				auto query = optionalString(entry, "query");
				auto handle = optionalString(entry, "handle");
				auto playedAs = optionalString(entry, "playedAs");
				auto name = optionalString(entry, "name");
				if (query.empty()) continue;
				if (!handle.empty()) {
					auto found = _byHandle.find(handle);
					if (found != _byHandle.end()) { _byQuery.insert_or_assign(query, found->second); continue; }
				}
				if (!playedAs.empty()) {
					auto found = _byPlayedAs.find(playedAs);
					if (found != _byPlayedAs.end()) { _byQuery.insert_or_assign(query, found->second); continue; }
				}
				auto found = _byName.find(name);
				if (found != _byName.end()) _byQuery.insert_or_assign(query, found->second);
			}
		return true;
	} catch (...) {
		return false;
	}
}

void RelativeRatings::_downloadProfiles(const std::vector<std::string> &profiles)
{
	std::error_code fsError;
	std::filesystem::create_directories(_cachePath, fsError);
	const auto cacheFileFor = [&](const std::string &name) {
		std::ostringstream out;
		out << std::hex << std::setw(16) << std::setfill('0') << std::hash<std::string>{}(name) << ".json";
		return std::filesystem::path(_cachePath) / out.str();
	};
	std::vector<std::string> pending;
	for (const auto &profile : profiles) {
		const auto path = cacheFileFor(profile);
		const auto written = cacheTime(path);
		if (written) {
			try {
				std::ifstream file(path, std::ios::binary | std::ios::ate);
				auto length = file.tellg();
				if (file && length > 0 && static_cast<std::uint64_t>(length) <= 64u * 1024u) {
					std::string json(static_cast<std::size_t>(length), '\0');
					file.seekg(0);
					file.read(json.data(), static_cast<std::streamsize>(length));
					if (file) _publish(json);
				}
			} catch (...) {}
		}
		constexpr std::uint64_t cacheLifetime = 6000000000ULL;
		if (!written || fileTimeNow() < written || fileTimeNow() - written >= cacheLifetime)
			pending.push_back(profile);
		else {
			std::lock_guard<std::mutex> lock(_mutex);
			_profileChecked[profile] = written;
		}
	}
	_debugLog("profile batch requested=" + std::to_string(profiles.size()) + " cache_miss=" + std::to_string(pending.size()));
	if (pending.empty()) return;

	std::string lastFetchInfo;
	auto fetch = [&](const std::string &url, const std::string *postBody, std::string &body, const std::string &label) {
		CURL *curl = curl_easy_init();
		if (!curl) {
			lastFetchInfo = label + " curl init failed";
			return false;
		}
		DownloadBuffer response{{}, &_enabled, &_stopping};
		curl_slist *headers = nullptr;
		if (postBody) headers = curl_slist_append(headers, "Content-Type: application/json");
		char errorBuffer[CURL_ERROR_SIZE]{};
		const auto caPath = std::filesystem::path(_caPath).string();
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
		curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
		curl_easy_setopt(curl, CURLOPT_CAINFO, caPath.c_str());
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "SokuLobbies-RelativeRatings/1.0");
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveBody);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transferProgress);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &response);
		if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		if (postBody) {
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody->c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(postBody->size()));
		}
		const auto result = curl_easy_perform(curl);
		long code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		lastFetchInfo = label + " result=" + curl_easy_strerror(result) + " http=" + std::to_string(code) +
			" bytes=" + std::to_string(response.body.size());
		if (result != CURLE_OK && errorBuffer[0])
			lastFetchInfo += " detail=" + std::string(errorBuffer);
		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);
		if (result != CURLE_OK || code != 200) return false;
		body = std::move(response.body);
		return !body.empty();
	};
	auto queryBatch = [&](const std::vector<std::string> &names, std::string &body) {
		Json request;
		request["names"] = names;
		const auto postBody = request.dump();
		return fetch("https://soku.cleista.cc/api/v1/ratings/players", &postBody, body, "ratings/players POST");
	};

	std::string body;
	if (!queryBatch(pending, body)) {
		_debugLog("batch request failed: " + lastFetchInfo + " ca_bundle_exists=" +
			(std::filesystem::exists(_caPath) ? "true" : "false"));
		std::lock_guard<std::mutex> lock(_mutex);
		for (const auto &name : pending) _profileChecked[name] = fileTimeNow() - 5400000000ULL;
		return;
	}
	const auto now = fileTimeNow();
	const bool published = _publish(body);
	std::unordered_set<std::string> foundNames;
	std::vector<std::pair<std::string, std::string>> aliases;
	std::size_t responseRecords = 0;
	try {
		auto root = Json::parse(body);
		if (root.contains("players") && root["players"].is_array()) {
			responseRecords = root["players"].size();
			for (const auto &entry : root["players"]) {
				const auto query = optionalString(entry, "query");
				const auto handle = optionalString(entry, "handle");
				if (!query.empty()) foundNames.insert(query);
				if (!query.empty() && !handle.empty()) aliases.emplace_back(query, handle);
			}
		}
	} catch (...) {}
	_debugLog("batch response: " + lastFetchInfo + " records=" + std::to_string(responseRecords) +
		" published=" + (published ? "true" : "false"));
	std::size_t aliasAttempts = 0;
	std::size_t aliasFailures = 0;
	for (const auto &name : pending) {
		if (foundNames.count(name)) continue;
		CURL *curl = curl_easy_init();
		if (!curl) continue;
		char *encoded = curl_easy_escape(curl, name.c_str(), static_cast<int>(name.size()));
		std::string mapping;
		if (encoded) {
			++aliasAttempts;
			if (!fetch("https://soku.cleista.cc/api/names/profiles?name=" + std::string(encoded), nullptr, mapping, "names/profiles GET"))
				++aliasFailures;
		}
		curl_free(encoded);
		curl_easy_cleanup(curl);
		try {
			auto root = Json::parse(mapping);
			auto item = root.find(name);
			if (item != root.end() && item->is_object()) {
				auto handle = optionalString(*item, "handle");
				if (!handle.empty()) aliases.emplace_back(name, std::move(handle));
			}
		} catch (...) {}
	}
	if (aliasFailures)
		_debugLog("profile alias fallback failures=" + std::to_string(aliasFailures) + "/" + std::to_string(aliasAttempts) +
			" last=" + lastFetchInfo);
	if (!aliases.empty()) {
		std::vector<std::string> handles;
		for (const auto &alias : aliases) handles.push_back("@" + alias.second);
		std::string handleBody;
		if (queryBatch(handles, handleBody)) {
			const bool handlePublished = _publish(handleBody);
			_debugLog("handle batch response: " + lastFetchInfo + " published=" + (handlePublished ? "true" : "false"));
			try {
				auto root = Json::parse(handleBody);
				if (root.contains("players") && root["players"].is_array()) {
					std::lock_guard<std::mutex> lock(_mutex);
					for (const auto &entry : root["players"]) {
						auto query = optionalString(entry, "query");
						if (query.size() > 1 && query[0] == '@')
							for (const auto &alias : aliases)
								if (query.substr(1) == alias.second) {
									auto found = _byHandle.find(alias.second);
									if (found != _byHandle.end()) {
										_byQuery.insert_or_assign(alias.first, found->second);
										_profileHandles[alias.first] = alias.second;
									}
								}
					}
				}
			} catch (...) {}
		}
	}
	for (const auto &name : pending) {
		std::lock_guard<std::mutex> lock(_mutex);
		_profileChecked[name] = foundNames.count(name) ? now : now - 5400000000ULL;
	}
	_debugLog("batch complete requested=" + std::to_string(pending.size()) + " matched=" + std::to_string(foundNames.size()) +
		" aliases=" + std::to_string(aliases.size()) + " rating_data_loaded=" + (hasData() ? "true" : "false"));
	// Persist each response row separately so future visits only read the names requested.
	try {
		auto root = Json::parse(body);
		if (root.contains("players") && root["players"].is_array())
			for (const auto &entry : root["players"]) {
				auto query = optionalString(entry, "query");
				if (query.empty()) continue;
				Json single;
				single["players"] = Json::array({entry});
				auto file = cacheFileFor(query);
				std::ofstream output(file, std::ios::binary | std::ios::trunc);
				auto serialized = single.dump();
				output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
			}
	} catch (...) {}
}

void RelativeRatings::_run()
{
	_loadMappings();
	_debugLog(std::string("worker started ca_bundle_exists=") + (std::filesystem::exists(_caPath) ? "true" : "false"));
	while (!_stopping.load(std::memory_order_relaxed)) {
		if (!_enabled.load(std::memory_order_relaxed)) {
			std::unique_lock<std::mutex> lock(_mutex);
			_wake.wait(lock, [this] { return _stopping.load(std::memory_order_relaxed) || _enabled.load(std::memory_order_relaxed); });
			continue;
		}
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_wake.wait_for(lock, std::chrono::milliseconds(150), [this] {
				return _stopping.load(std::memory_order_relaxed) || !_enabled.load(std::memory_order_relaxed);
			});
		}
		std::vector<std::string> pendingProfiles;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			pendingProfiles.swap(_pendingProfiles);
		}
		if (!pendingProfiles.empty()) {
			_downloadProfiles(pendingProfiles);
			continue;
		}
		std::unique_lock<std::mutex> lock(_mutex);
		_wake.wait(lock, [this] {
			return _stopping.load(std::memory_order_relaxed) || !_enabled.load(std::memory_order_relaxed) || !_pendingProfiles.empty();
		});
	}
}
