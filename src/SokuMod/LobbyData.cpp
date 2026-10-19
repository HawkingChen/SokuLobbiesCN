//
// Created by PinkySmile on 10/11/2022.
//

#include <fstream>
#include <filesystem>
#include <Packet.hpp>
#include "nlohmann/json.hpp"
#include "data.hpp"
#include "LobbyData.hpp"
#include "dinput.h"
#include "HttpResponseBuffer.hpp"
#include "ClientTransport.hpp"

#define ACH_GET_TOTAL_TIME 360
#define ACH_GET_FADE_IN_ANIM 45
#define ACH_GET_FADE_OUT_ANIM 45

std::unique_ptr<LobbyData> lobbyData;

extern std::map<unsigned int, Character> characters;

void LobbyData::saveStats() try
{
	auto path = std::filesystem::path(profileFolderPath) / "stats.dat";
	auto backup = std::filesystem::path(path.wstring() + L".backup");
	auto temporary = std::filesystem::path(path.wstring() + L".tmp");
	std::ofstream stream{temporary, std::fstream::binary | std::fstream::trunc};

	if (stream.fail()) {
		fprintf(stderr, "Cannot open temporary stats file %s: %s\n", temporary.string().c_str(), strerror(errno));
		return;
	}

	CharacterGameStats::save(stream, characterGames);
	stream.flush();
	bool failed = stream.fail();
	stream.close();
	if (failed) {
		fprintf(stderr, "Error writing temporary stats file: %s\n", errno ? strerror(errno) : "Unknown error");
		std::error_code ec;
		std::filesystem::remove(temporary, ec);
		return;
	}
	std::error_code existsError;
	bool primaryExists = std::filesystem::exists(path, existsError);
	if (existsError)
		fprintf(stderr, "Cannot inspect stats file before backup: %s\n", existsError.message().c_str());
	else if (primaryExists && !CopyFileW(path.wstring().c_str(), backup.wstring().c_str(), FALSE))
		fprintf(stderr, "Cannot update stats backup: Windows error %lu\n", GetLastError());
	if (!MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		fprintf(stderr, "Cannot replace stats file: Windows error %lu\n", GetLastError());
		std::error_code ec;
		std::filesystem::remove(temporary, ec);
	}
}
catch (const std::exception &e) { fprintf(stderr, "Cannot save character game counts: %s\n", e.what()); }

void LobbyData::_loadStats()
{
	auto path = std::filesystem::path(profileFolderPath) / "stats.dat";
	auto backup = std::filesystem::path(path.wstring() + L".backup");
	std::string primaryError;
	std::string backupError;
	auto tryLoad = [this](const std::filesystem::path &candidate, std::string &error) {
		try {
			std::error_code ec;
			bool exists = std::filesystem::exists(candidate, ec);
			if (ec)
				throw std::runtime_error("Cannot inspect file: " + ec.message());
			if (!exists) {
				error = "file does not exist";
				return false;
			}
			this->_loadStatsFile(candidate);
			return true;
		} catch (const std::exception &e) {
			error = e.what();
			this->_clearStats();
			return false;
		}
	};

	if (tryLoad(path, primaryError))
		return;
	fprintf(stderr, "Failed to load %s: %s\n", path.string().c_str(), primaryError.c_str());
	if (tryLoad(backup, backupError)) {
		fprintf(stderr, "Recovered statistics from %s\n", backup.string().c_str());
		auto temporary = std::filesystem::path(path.wstring() + L".recovering");
		if (
			CopyFileW(backup.wstring().c_str(), temporary.wstring().c_str(), FALSE) &&
			MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
		)
			return;
		fprintf(stderr, "Loaded stats backup but could not restore primary file: Windows error %lu\n", GetLastError());
		std::error_code ec;
		std::filesystem::remove(temporary, ec);
		return;
	}
	fprintf(stderr, "Failed to load %s: %s. Starting with empty statistics.\n", backup.string().c_str(), backupError.c_str());
	this->_clearStats();
	auto quarantine = [](const std::filesystem::path &source, const std::filesystem::path &target) {
		std::error_code ec;
		bool exists = std::filesystem::exists(source, ec);
		if (ec)
			fprintf(stderr, "Cannot inspect %s for quarantine: %s\n", source.string().c_str(), ec.message().c_str());
		else if (exists && !MoveFileExW(source.wstring().c_str(), target.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			fprintf(stderr, "Cannot quarantine %s: Windows error %lu\n", source.string().c_str(), GetLastError());
	};
	quarantine(path, std::filesystem::path(path.wstring() + L".corrupt"));
	quarantine(backup, std::filesystem::path(backup.wstring() + L".corrupt"));
}

void LobbyData::_clearStats()
{
	characterGames.clear();
}

void LobbyData::_loadStatsFile(const std::filesystem::path &path)
{
	std::ifstream stream{path, std::ios::binary};
	if (!stream) throw std::invalid_argument("Cannot open stats file");
	const bool legacy = CharacterGameStats::read(stream, 4) != CharacterGameStats::MAGIC;
	stream.seekg(0);
	characterGames = CharacterGameStats::load(stream);
	if (legacy) {
		// Preserve the original valid legacy data before the first counts-only save.
		std::error_code error;
		std::filesystem::copy_file(path, path.parent_path() / "stats.dat.legacy",
			std::filesystem::copy_options::skip_existing, error);
		if (error) fprintf(stderr, "Cannot archive legacy statistics: %s\n", error.message().c_str());
	}
}

void LobbyData::recordCharacterGame(unsigned char character, bool randomSelection)
{
	auto &games = characterGames[character];
	if (games != UINT64_MAX) ++games;
	if (randomSelection && character != SokuLib::CHARACTER_RANDOM) {
		auto &randomGames = characterGames[SokuLib::CHARACTER_RANDOM];
		if (randomGames != UINT64_MAX) ++randomGames;
	}
	// Persistence and optional rewards must never unwind into the game hook.
	try { saveStats(); } catch (const std::exception &e) { fprintf(stderr, "Cannot save game counts: %s\n", e.what()); }
	try { _grantStatsAchievements(); saveAchievements(); }
	catch (const std::exception &e) { fprintf(stderr, "Cannot update game-count achievements: %s\n", e.what()); }
}

static void loadTexture(SokuLib::DrawUtils::Texture &container, const char *path)
{
	int text = 0;
	SokuLib::Vector2u size;
	int *ret = SokuLib::textureMgr.loadTexture(&text, path, &size.x, &size.y);

	printf("Loading texture %s\n", path);
	if (!ret || !text) {
		puts("Couldn't load texture...");
		MessageBoxA(SokuLib::window, ("Cannot load game asset " + std::string(path)).c_str(), "Game texture loading failed", MB_ICONWARNING);
	}
	container.setHandle(text, size);
}

static inline void loadTexture(SokuLib::DrawUtils::Sprite &container, const char *path)
{
	loadTexture(container.texture, path);
	container.rect.width = container.texture.getSize().x;
	container.rect.height = container.texture.getSize().y;
	container.setSize(container.texture.getSize());
}

void LobbyData::_loadAvatars()
{
	std::filesystem::path folder = profileFolderPath;
	auto path = folder / "assets/avatars/list.json";
	auto path2 = folder / "assets/avatars/order.json";
	std::ifstream stream{path};
	nlohmann::json j;
	nlohmann::json j2;
	std::vector<std::string> order;
	std::map<std::string, std::string> sprites;

	if (stream.fail())
		throw std::runtime_error("Cannot open file " + path.string() + ": " + strerror(errno));
	printf("Loading %s\n", path.string().c_str());
	stream >> j;
	stream.close();

	std::ifstream stream2{path2};

	if (stream2.fail())
		throw std::runtime_error("Cannot open file " + path2.string() + ": " + strerror(errno));
	printf("Loading %s\n", path2.string().c_str());
	stream2 >> j2;
	stream2.close();
	order = j2.get<std::vector<std::string>>();

	this->avatars.reserve(j.size());
	for (auto &val : j) {
		// Strings in the array are used as comments
		if (val.is_string())
			continue;
		this->avatars.emplace_back();

		auto &avatar = this->avatars.back();

		avatar.id = this->avatars.size() - 1;
		avatar.code = val["id"];
		avatar.name = val["name"];
		avatar.scale = val["scale"];
		avatar.nbAnimations = val["animations"];
		avatar.animationsStep = val["anim_step"];
		avatar.accessoriesPlacement = val["accessories"];
		if (val.contains("animation_style"))
			avatar.animationStyle = val["animation_style"];
		if (val.contains("hidden"))
			avatar.hidden = val["hidden"];
		sprites[avatar.code] = val["spritesheet"];
		avatar.sprite.texture.loadFromFile((std::filesystem::path(profileFolderPath) / val["spritesheet"].get<std::string>()).string().c_str());
		avatar.sprite.rect.width = avatar.sprite.texture.getSize().x / avatar.nbAnimations;
		avatar.sprite.rect.height = avatar.sprite.texture.getSize().y / 2;
		avatar.sprite.setSize({
			static_cast<unsigned int>(avatar.sprite.rect.width * avatar.scale),
			static_cast<unsigned int>(avatar.sprite.rect.height * avatar.scale)
		});
		if (std::find(order.begin(), order.end(), avatar.code) == order.end())
			throw std::invalid_argument("Avatar ID is not in the order list: " + avatar.code);
	}
	std::sort(this->avatars.begin(), this->avatars.end(), [&order](const Avatar &a, const Avatar &b){
		return std::find(order.begin(), order.end(), a.code) < std::find(order.begin(), order.end(), b.code);
	});
	for (auto &avatar : this->avatars) {
		if (this->avatarsByCode.find(avatar.code) != this->avatarsByCode.end())
			throw std::invalid_argument("Duplicate avatar ID found: " + avatar.code);
		this->avatarsByCode[avatar.code] = &avatar;
	}
	printf("There are %zu avatars\n", this->avatars.size());
}

void LobbyData::_loadAchievements()
{
	std::filesystem::path folder = profileFolderPath;
	auto path = folder / "assets/achievements.json";
	auto path2 = folder / "assets/ach_order.json";
	std::ifstream stream{path};
	nlohmann::json j;
	nlohmann::json j2;
	std::vector<std::string> order;
	std::map<std::string, Achievement *> achs;
	char *buffer;
	int index = 0;

	printf("Loading %s\n", path.string().c_str());
	if (stream.fail())
		throw std::runtime_error("Cannot open file " + path.string() + ": " + strerror(errno));
	stream >> j;
	stream.close();

	std::ifstream stream2{path2};

	printf("Loading %s\n", path2.string().c_str());
	if (stream2.fail())
		throw std::runtime_error("Cannot open file " + path2.string() + ": " + strerror(errno));
	stream2 >> j2;
	stream2.close();
	order = j2.get<std::vector<std::string>>();

	this->achievements.reserve(j.size());
	for (auto &val : j) {
		// Strings in the array are used as comments
		if (val.is_string())
			continue;

		this->achievements.emplace_back();

		auto &achievement = this->achievements.back();
		SokuLib::Vector2i size;

		achievement.code = val["id"];
		achievement.description = val["description"];
		achievement.name = val["name"];
		achievement.requirement = val["requirement"];
		achievement.hidden = val["hidden"];
		achievement.category = val["category"];
		achievement.rewards = val["rewards"].get<std::vector<nlohmann::json>>();

		achievement.nameSpriteTitle.texture.createFromText(achievement.name.c_str(), this->getFont(18), {400, 22}, &size);
		achievement.nameSpriteTitle.rect.width = size.x;
		achievement.nameSpriteTitle.rect.height = size.y;
		achievement.nameSpriteTitle.setSize(size.to<unsigned>());
		achievement.nameSpriteTitle.setPosition(SokuLib::Vector2i{315 + 151, 125} - SokuLib::Vector2i{size.x / 2, 0});

		achievement.nameSpriteFull.texture.createFromText(achievement.name.c_str(), this->getFont(14), {400, 20});
		achievement.nameSpriteFull.rect.width = achievement.nameSpriteFull.texture.getSize().x;
		achievement.nameSpriteFull.rect.height = achievement.nameSpriteFull.texture.getSize().y;
		achievement.nameSpriteFull.setSize({
			static_cast<unsigned int>(achievement.nameSpriteFull.rect.width),
			static_cast<unsigned int>(achievement.nameSpriteFull.rect.height)
		});
		achievement.nameSpriteFull.setPosition({0, -20});

		achievement.nameSprite.texture.createFromText(achievement.name.c_str(), this->getFont(12), {400, 20});
		achievement.nameSprite.rect.width = achievement.nameSprite.texture.getSize().x;
		achievement.nameSprite.rect.height = achievement.nameSprite.texture.getSize().y;
		achievement.nameSprite.setSize({
			static_cast<unsigned int>(achievement.nameSprite.rect.width),
			static_cast<unsigned int>(achievement.nameSprite.rect.height)
		});
		achievement.nameSprite.setPosition({0, -20});

		achievement.descSprite.texture.createFromText(achievement.description.c_str(), this->getFont(14), {400, 100}, &size);
		achievement.descSprite.rect.width = size.x;
		achievement.descSprite.rect.height = size.y;
		achievement.descSprite.setSize(size.to<unsigned>());
		achievement.descSprite.setPosition({0, -20});
		achievement.descSprite.tint = SokuLib::Color{0x80, 0xFF, 0x80};

		if (achs.find(achievement.code) != achs.end())
			throw std::invalid_argument("Duplicate achievement ID '" + achievement.code + "'");
		achs[achievement.code] = &achievement;
	}
	for (auto &o : order)
		if (achs.find(o) == achs.end())
			throw std::invalid_argument("Achievement ID '" + o + "' in order file doesn't exist.");
	for (auto &[id, ach] : achs)
		if (std::find(order.begin(), order.end(), id) == order.end())
			throw std::invalid_argument("Achievement ID '" + id + "' isn't in order file.");
	std::sort(this->achievements.begin(), this->achievements.end(), [&order](const Achievement &a, const Achievement &b){
		return std::find(order.begin(), order.end(), a.code) < std::find(order.begin(), order.end(), b.code);
	});

	size_t nbBytes = std::ceil(j.size() / 7.f);

	stream.open(folder / "achievements.dat", std::fstream::binary);
	buffer = new char[nbBytes];
	memset(buffer, 0, nbBytes);
	if (stream) {
		stream.read(buffer, nbBytes);
		stream.close();
	} else
		printf("Cannot open file %s: %s\n", (folder / "achievements.dat").string().c_str(), strerror(errno));
	for (size_t i = 0; i < nbBytes; i++)
		if (buffer[i] & (1 << (i % 8))) {
			this->achievementsLocked = true;
			break;
		}

	bool warning = false;

	for (auto &achievement : this->achievements) {
		auto arrayIndex = index / 7;
		auto bit = index % 7;
		auto flagBit = arrayIndex % 8;

		bit += bit >= flagBit;
		achievement.awarded = (buffer[arrayIndex] & (1 << bit)) != 0;
		if (achievement.name.size() > 35) {
			printf("Warning: Too long achievement name '%s'\n", achievement.name.c_str());
			warning = true;
		}
		this->achievementByRequ[achievement.requirement["type"]].push_back(&achievement);
		if (!achievement.rewards.empty()) {
			for (auto &reward : achievement.rewards) {
				auto type = reward["type"];

				if (type == "title");
				else if (type == "avatar") {
					std::string id = reward["id"];
					auto it = std::find_if(this->avatars.begin(), this->avatars.end(), [&id](Avatar &a){ return a.code == id; });

					if (it == this->avatars.end())
						throw std::invalid_argument("Invalid avatar " + id);
					it->requirement = &achievement;
				} else if (type == "emote") {
					std::string id = reward["name"];
					auto it = this->emotesByName.find(id);

					if (it == this->emotesByName.end())
						throw std::invalid_argument("Invalid emote " + id);
					it->second->requirement = &achievement;
				} else if (type == "prop") {

				} else if (type == "accessory") {

				}
			}
		}
		index++;
	}
	if (warning)
		MessageBox(SokuLib::window, ".", ".", MB_ICONWARNING);
	delete[] buffer;
	printf("There are %zu achievements and they are %slocked\n", this->achievements.size(), (this->achievementsLocked ? "" : "un"));
}

void LobbyData::saveAchievements()
{
	std::filesystem::path folder = profileFolderPath;
	size_t nbBytes = std::ceil(this->achievements.size() / 7.f);
	auto buffer = new char[nbBytes];
	auto index = 0;

	memset(buffer, 0, nbBytes);
	if (this->achievementsLocked)
		for (size_t i = 0; i < nbBytes; i++)
			buffer[i] |= 1 << (i % 8);
	for (auto &achievement : this->achievements) {
		auto arrayIndex = index / 7;
		auto bit = index % 7;
		auto flagBit = arrayIndex % 8;

		bit += bit >= flagBit;
		if (achievement.awarded)
			buffer[arrayIndex] |= 1 << bit;
		index++;
	}

	std::ofstream stream{folder / "achievements.dat", std::fstream::binary};

	if (stream) {
		stream.write(buffer, nbBytes);
		stream.close();
	}
	delete[] buffer;
}

void LobbyData::_loadBackgrounds()
{
	std::filesystem::path folder = profileFolderPath;
	auto path = folder / "assets/backgrounds/list.json";
	std::ifstream stream{path};
	nlohmann::json j;

	if (stream.fail())
		throw std::runtime_error("Cannot open file " + path.string() + ": " + strerror(errno));
	printf("Loading %s\n", path.string().c_str());
	stream >> j;
	stream.close();
	this->backgrounds.reserve(j.size());
	for (auto &val : j) {
		this->backgrounds.emplace_back();

		auto &bg = this->backgrounds.back();

		bg.id = this->backgrounds.size() - 1;
		bg.size.x = val["width"];
		bg.size.y = val["height"];
		bg.startX = val["spawn"]["x"];
		bg.startPlatform = val["spawn"]["platform"];
		if (val.contains("clock")) {
			ClockLayer clock{nullptr, nullptr, nullptr, {0, 0}};
			auto &cl = val["clock"];

			clock.center.x = cl["center"]["x"];
			clock.center.y = cl["center"]["y"];
			if (!cl["hour"].is_null()) {
				bg.images.emplace_back();
				clock.hour = &bg.images.back();
				clock.hour->texture.loadFromFile((folder / cl["hour"].get<std::string>()).string().c_str());
				clock.hour->setSize(clock.hour->texture.getSize());
				clock.hour->rect.width = clock.hour->getSize().x;
				clock.hour->rect.height = clock.hour->getSize().y;
			}
			if (!cl["minute"].is_null()) {
				bg.images.emplace_back();
				clock.minute = &bg.images.back();
				clock.minute->texture.loadFromFile((folder / cl["minute"].get<std::string>()).string().c_str());
				clock.minute->setSize(clock.minute->texture.getSize());
				clock.minute->rect.width = clock.minute->getSize().x;
				clock.minute->rect.height = clock.minute->getSize().y;
			}
			if (!cl["second"].is_null()) {
				bg.images.emplace_back();
				clock.second = &bg.images.back();
				clock.second->texture.loadFromFile((folder / cl["second"].get<std::string>()).string().c_str());
				clock.second->setSize(clock.second->texture.getSize());
				clock.second->rect.width = clock.second->getSize().x;
				clock.second->rect.height = clock.second->getSize().y;
			}
			bg.clock = clock;
		}
		bg.layers.reserve(val["layers"].size());
		for (auto &layer_j : val["layers"]) {
			bg.layers.emplace_back();

			auto &layer = bg.layers.back();
			std::string img = layer_j;

			if (img == "system") {
				layer.image = nullptr;
				layer.type = LAYERTYPE_SYSTEM;
				continue;
			}
			if (img == "clock") {
				layer.image = nullptr;
				layer.type = LAYERTYPE_CLOCK;
				continue;
			}
			bg.images.emplace_back();
			layer.type = LAYERTYPE_IMAGE;
			layer.image = &bg.images.back();
			layer.image->texture.loadFromFile((folder / img).string().c_str());
			layer.image->setSize(layer.image->texture.getSize());
			layer.image->rect.width = layer.image->getSize().x;
			layer.image->rect.height = layer.image->getSize().y;
		}
		bg.platforms.reserve(val["platforms"].size());
		for (auto &platform_j : val["platforms"]) {
			bg.platforms.emplace_back();

			auto &platform = bg.platforms.back();

			platform.pos.x = platform_j["left"];
			platform.pos.y = platform_j["top"];
			platform.width = platform_j["width"];
		}
		bg.arcades.reserve(val["arcades"].size());
		for (auto &arcade_j : val["arcades"]) {
			bg.arcades.emplace_back();

			auto &arcade = bg.arcades.back();

			arcade.pos.x = arcade_j["x"];
			arcade.pos.y = arcade_j["y"];
			arcade.old = arcade_j.contains("old") && arcade_j["old"];
			arcade.special = arcade_j.contains("special") && arcade_j["special"];
		}
		bg.elevators.reserve(val["elevators"].size());
		for (auto &elevator_j : val["elevators"]) {
			bg.elevators.emplace_back();

			auto &elevator = bg.elevators.back();

			elevator.pos.x = elevator_j["x"];
			elevator.pos.y = elevator_j["y"];
			elevator.noIndicator = elevator_j.contains("no_indicator") && elevator_j["no_indicator"];
			elevator.skin = elevator_j["skin"];
			elevator.hidden = elevator_j.contains("hidden") && elevator_j["hidden"];
			if (!elevator_j["left_link"].is_null())
				elevator.leftLink = {elevator_j["left_link"], 0};
			if (!elevator_j["right_link"].is_null())
				elevator.rightLink = {elevator_j["right_link"], 0};
			if (!elevator_j["up_link"].is_null())
				elevator.upLink = {elevator_j["up_link"], 0};
			if (!elevator_j["down_link"].is_null())
				elevator.downLink = {elevator_j["down_link"], 0};
		}
		for (auto &elevator : bg.elevators) {
			for (int j = 0; j < 4; j++) {
				auto &link = (&elevator.leftLink)[j];

				if (!link)
					continue;
				if (link->elevator >= bg.elevators.size())
					throw std::invalid_argument("Invalid link ID: " + std::to_string(link->elevator));

				auto &other = bg.elevators[link->elevator];

				for (unsigned i = 0; i < bg.platforms.size(); i++) {
					auto &platform = bg.platforms[i];

					if (platform.pos.y != other.pos.y)
						continue;
					if (platform.pos.x > other.pos.x)
						continue;
					if (platform.pos.x + platform.width < other.pos.x)
						continue;
					link->platform = i;
					goto OK;
				}
				throw std::invalid_argument("Elevator " + std::to_string(link->elevator) + " is not on any platform");
			OK:
				continue;
			}
		}
	}
	printf("There are %zu backgrounds\n", this->avatars.size());
}

void LobbyData::_loadEmotes()
{
	std::filesystem::path folder = profileFolderPath;
	auto path = folder / "assets/emotes/list.json";
	std::ifstream stream{path};
	nlohmann::json j;

	if (stream.fail())
		throw std::runtime_error("Cannot open file " + path.string() + ": " + strerror(errno));
	printf("Loading %s\n", path.string().c_str());
	stream >> j;
	stream.close();
	this->emotes.reserve(j.size());
	for (auto &val : j) {
		this->emotes.emplace_back();

		auto &emote = this->emotes.back();

		emote.id = this->emotes.size() - 1;
		emote.filepath = val["path"];
		emote.alias = val["alias"].get<std::vector<std::string>>();
		emote.sprite.texture.loadFromFile((folder / emote.filepath).string().c_str());
		emote.sprite.setSize({EMOTE_SIZE, EMOTE_SIZE});
		emote.sprite.rect.width = emote.sprite.texture.getSize().x;
		emote.sprite.rect.height = emote.sprite.texture.getSize().y;
		for (auto &alias : emote.alias) {
			auto it = this->emotesByName.find(alias);

			if (it != this->emotesByName.end())
				throw std::runtime_error("Duplicate alias " + alias);
			this->emotesByName[alias] = &emote;
		}
	}
	printf("There are %zu emotes (%zu different alias)\n", this->emotes.size(), this->emotesByName.size());
}

void LobbyData::_loadFlags()
{
	std::filesystem::path folder = profileFolderPath;
	auto path = folder / "assets/flags/list.json";
	std::ifstream stream{path};
	nlohmann::json j;

	if (stream.fail())
		throw std::runtime_error("Cannot open file " + path.string() + ": " + strerror(errno));
	printf("Loading %s\n", path.string().c_str());
	stream >> j;
	stream.close();
	for (auto &val : j.items()) {
		auto &flag = this->flags[val.key()];

		flag = std::make_unique<SokuLib::DrawUtils::Sprite>();
		flag->texture.loadFromFile((folder / "assets/flags" / val.value().get<std::string>()).string().c_str());
		flag->setSize({EMOTE_SIZE, EMOTE_SIZE});
		flag->rect.width = flag->texture.getSize().x;
		flag->rect.height = flag->texture.getSize().y;
	}
	printf("There are %zu flags\n", this->flags.size());
}

SokuLib::DrawUtils::TextureRect parseTextureRect(nlohmann::json &val)
{
	SokuLib::DrawUtils::TextureRect rect;

	rect.left = val["left"];
	rect.top = val["top"];
	rect.width = val["width"];
	rect.height = val["height"];
	return rect;
}

LobbyData::DoorOpenAnimation parseDoorAnim(const std::string &anim)
{
	if (anim == "slide")
		return LobbyData::DOOR_OPEN_SLIDE;
	if (anim == "rotate")
		return LobbyData::DOOR_OPEN_ROTATE;
	throw std::invalid_argument("Invalid door open animation provided: " + anim);
}

void LobbyData::_loadElevators()
{
	std::filesystem::path folder = profileFolderPath;
	auto path = folder / "assets/elevators/list.json";
	std::ifstream stream{path};
	nlohmann::json j;

	if (stream.fail())
		throw std::runtime_error("Cannot open file " + path.string());
	printf("Loading %s\n", path.string().c_str());
	stream >> j;
	stream.close();
	this->elevators.reserve(j.size());
	for (auto &val : j) {
		this->elevators.emplace_back();

		auto &skin = this->elevators.back();

		skin.file = val["sprite"];

		auto file = std::filesystem::path(profileFolderPath) / skin.file;

		skin.doorOffset.x = val["door_offset"]["x"];
		skin.doorOffset.y = val["door_offset"]["y"];
		skin.cage = parseTextureRect(val["cage"]);
		skin.indicator = parseTextureRect(val["indicator"]);
		skin.arrow = parseTextureRect(val["arrow"]);
		skin.doorLeft = parseTextureRect(val["doors"][0]);
		skin.doorRight = parseTextureRect(val["doors"][1]);
		skin.anim = parseDoorAnim(val["open_anim"]);

		skin.sprite.texture.loadFromFile(file.string().c_str());
	}
}

static void extractArcadeAnimation(LobbyData::ArcadeAnimation &animation, const nlohmann::json &j)
{
	animation.file = j["file"];
	animation.size.x = j["sizeX"];
	animation.size.y = j["sizeY"];
	animation.frameRate = j["rate"];
	animation.frameCount = j["frames"];
	animation.loop = j.contains("loop") && j["loop"];
	animation.sprite.texture.loadFromFile((std::filesystem::path(profileFolderPath) / animation.file).string().c_str());
	animation.sprite.setSize({32, 24});
	animation.sprite.rect.width = animation.size.x;
	animation.sprite.rect.height = animation.size.y;
	animation.tilePerLine = animation.sprite.texture.getSize().x / animation.size.x;
}

void LobbyData::_loadArcades()
{
	std::filesystem::path folder = profileFolderPath;
	auto path = folder / "assets/arcades/list.json";
	std::ifstream stream{path};
	nlohmann::json j;

	if (stream.fail())
		throw std::runtime_error("Cannot open file " + path.string());
	printf("Loading %s\n", path.string().c_str());
	stream >> j;
	stream.close();
	extractArcadeAnimation(this->arcades.intro, j["intro"]);
	extractArcadeAnimation(this->arcades.select, j["select"]);
	extractArcadeAnimation(this->arcades.hostlist, j["hostlist"]);
	this->arcades.game.reserve(j["games"].size());
	for (auto &val : j["games"]) {
		this->arcades.game.emplace_back();
		extractArcadeAnimation(this->arcades.game.back(), val);
	}
	this->arcades.skins.reserve(j["arcades"].size());
	for (auto &val : j["arcades"]) {
		this->arcades.skins.emplace_back();

		auto &skin = this->arcades.skins.back();

		skin.file = val["file"];

		auto file = std::filesystem::path(profileFolderPath) / skin.file;

		skin.animationOffsets.x = val["offsetX"];
		skin.animationOffsets.y = val["offsetY"];
		skin.frameRate = val["rate"];
		skin.frameCount = val["frames"];
		skin.sprite.texture.loadFromFile(file.string().c_str());
		skin.sprite.setSize({skin.sprite.texture.getSize().x / skin.frameCount, skin.sprite.texture.getSize().y});
		skin.sprite.rect.width = skin.sprite.getSize().x;
		skin.sprite.rect.height = skin.sprite.getSize().y;

		skin.overlay.texture.loadFromFile(file.string().c_str());
		skin.overlay.setSize({
			skin.sprite.getSize().x * 12,
			skin.sprite.getSize().y * 12
		});
		skin.overlay.setPosition({
			128 - skin.animationOffsets.x * 12,
			48 - skin.animationOffsets.y * 12
		});
		skin.overlay.rect.width = skin.sprite.getSize().x;
		skin.overlay.rect.height = skin.sprite.getSize().y;
	}
}

size_t LobbyData::writeMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) noexcept
{
	return userp ? appendHttpResponse(static_cast<MemoryStruct *>(userp)->body, contents, size, nmemb) : 0;
}

LobbyData::LobbyData()
{
	this->_loadStats();
	this->_loadFlags();
	this->_loadAvatars();
	this->_loadBackgrounds();
	this->_loadEmotes();
	this->_loadArcades();
	this->_loadElevators();
	this->_loadAchievements();
	this->_grantStatsAchievements();
#ifdef _DEBUG
	this->_grantDebugAchievements();
#endif
	if (GetFileAttributesW(L".crash") != INVALID_FILE_ATTRIBUTES)
		this->_grantCrashAchievements();

	this->achHolder.getText.texture.createFromText("Achievement Unlocked!", this->getFont(16), {400, 20});
	this->achHolder.getText.setSize(this->achHolder.getText.texture.getSize());
	this->achHolder.getText.rect.width = this->achHolder.getText.getSize().x;
	this->achHolder.getText.rect.height = this->achHolder.getText.getSize().y;
	this->achHolder.getText.tint = SokuLib::Color::Yellow;
	this->achHolder.getText.setPosition((this->achHolder.getText.getSize() * -1).to<int>());

	this->achHolder.holder.texture.loadFromFile((std::filesystem::path(profileFolderPath) / "assets/ACHIEVEMENT_BAR.png").string().c_str());
	this->achHolder.holder.setSize(this->achHolder.holder.texture.getSize());
	this->achHolder.holder.rect.width = this->achHolder.holder.getSize().x;
	this->achHolder.holder.rect.height = this->achHolder.holder.getSize().y;
	this->achHolder.holder.setPosition((this->achHolder.holder.getSize() * -1).to<int>());

	this->achHolder.behindGear.texture.loadFromGame("data/menu/gear/4L-mid_S.png");
	this->achHolder.behindGear.setSize(this->achHolder.behindGear.texture.getSize());
	this->achHolder.behindGear.rect.width = this->achHolder.behindGear.getSize().x;
	this->achHolder.behindGear.rect.height = this->achHolder.behindGear.getSize().y;
	this->achHolder.behindGear.setPosition((this->achHolder.behindGear.getSize() * -1).to<int>());

	curl_global_init(CURL_GLOBAL_ALL);
	this->_relativeRatings = std::make_unique<RelativeRatings>(profileFolderPath);
	this->_relativeRatings->setEnabled(relativeRatingsEnabled);
}

LobbyData::~LobbyData()
{
	if (this->_relativeRatings)
		this->_relativeRatings->stop();
	this->saveStats();
	this->saveAchievements();
	for (auto &font : this->_fonts)
		font.second.destruct();
	curl_global_cleanup();
}

bool LobbyData::isLocked(const LobbyData::Emote &emote)
{
	return !this->achievementsLocked && emote.requirement && !emote.requirement->awarded;
}

bool LobbyData::isLocked(const LobbyData::Avatar &avatar)
{
	return !this->achievementsLocked && avatar.requirement && !avatar.requirement->awarded;
}

bool LobbyData::isLocked(const LobbyData::Background &background)
{
	return !this->achievementsLocked && background.requirement && !background.requirement->awarded;
}

void LobbyData::_loadFont(SokuLib::SWRFont &font, unsigned int size)
{
	SokuLib::FontDescription desc;
	bool hasEnglishPatch = (*(int *)0x411c64 == 1);

	desc.r1 = 255;
	desc.r2 = 255;
	desc.g1 = 255;
	desc.g2 = 255;
	desc.b1 = 255;
	desc.b2 = 255;
	desc.height = size + hasEnglishPatch * 2;
	desc.weight = FW_NORMAL;
	desc.italic = 0;
	desc.shadow = 1;
	desc.bufferSize = 1000000;
	desc.charSpaceX = 0;
	desc.charSpaceY = hasEnglishPatch * -2;
	desc.offsetX = 0;
	desc.offsetY = 0;
	desc.useOffset = 0;
	strcpy(desc.faceName, "MonoSpatialModSWR");
	desc.weight = FW_REGULAR;

	printf("Loading font of size %i (actual size %li)\n", size, desc.height);
	font.create();
	font.setIndirect(desc);
}

SokuLib::SWRFont &LobbyData::getFont(unsigned int size)
{
	auto it = this->_fonts.find(size);

	if (it != this->_fonts.end())
		return it->second;

	auto &font = this->_fonts[size];

	this->_loadFont(font, size);
	return font;
}

void LobbyData::update()
{
	if (this->achievementsLocked)
		this->achievementAwardQueue.clear();
	if (this->achievementAwardQueue.empty())
		return;
	if (this->_achTimer == 0)
		playSound(33);
	this->_achTimer++;
	if (this->_achTimer < ACH_GET_FADE_IN_ANIM)
		this->achHolder.behindGear.setPosition({
			320 - this->achHolder.holder.rect.width / 2 - this->achHolder.behindGear.rect.width / 2,
			480 - static_cast<int>(this->achHolder.behindGear.rect.height * this->_achTimer / ACH_GET_FADE_IN_ANIM) + (this->achHolder.behindGear.rect.height - this->achHolder.holder.rect.height) / 2
		});
	else if (this->_achTimer > ACH_GET_TOTAL_TIME - ACH_GET_FADE_OUT_ANIM)
		this->achHolder.behindGear.setPosition({
			320 - this->achHolder.holder.rect.width / 2 - this->achHolder.behindGear.rect.width / 2,
			480 - static_cast<int>(this->achHolder.behindGear.rect.height * (ACH_GET_TOTAL_TIME - this->_achTimer) / ACH_GET_FADE_OUT_ANIM) + (this->achHolder.behindGear.rect.height - this->achHolder.holder.rect.height) / 2
		});
	else
		this->achHolder.behindGear.setPosition({
			320 - this->achHolder.holder.rect.width / 2 - this->achHolder.behindGear.rect.width / 2,
			480 - this->achHolder.behindGear.rect.height + (this->achHolder.behindGear.rect.height - this->achHolder.holder.rect.height) / 2
		});

	auto &achievement = this->achievementAwardQueue.front();

	if (achievement->rewards.empty() || achievement->rewards[this->_reward]["type"] == "title") {
		this->achHolder.getText.setPosition(this->achHolder.holder.getPosition() + SokuLib::Vector2i{5, 5});
		this->achHolder.holder.setPosition(this->achHolder.behindGear.getPosition() + SokuLib::Vector2i{
			this->achHolder.behindGear.rect.width / 2,
			(this->achHolder.behindGear.rect.height - this->achHolder.holder.rect.height) / 2
		});
	} else {
		auto reward = achievement->rewards[this->_reward];
		auto type = reward["type"];

		if (this->_achTimer % 60 == 0) {
			this->_reward++;
			if (this->_reward >= achievement->rewards.size() || achievement->rewards[this->_reward]["type"] == "title")
				this->_reward = 0;
		}
		this->achHolder.holder.setPosition(this->achHolder.behindGear.getPosition() + SokuLib::Vector2i{
			this->achHolder.behindGear.rect.width * 3 / 4,
			(this->achHolder.behindGear.rect.height - this->achHolder.holder.rect.height) / 2
		});
		this->achHolder.getText.setPosition({
			static_cast<int>(this->achHolder.behindGear.getPosition().x + this->achHolder.behindGear.getSize().x + 4),
			this->achHolder.holder.getPosition().y + 5
		});
		if (type == "avatar") {
			auto &avatar = *this->avatarsByCode[reward["id"]];

			this->_animCtr++;
			if (this->_animCtr >= avatar.animationsStep) {
				this->_animCtr = 0;
				this->_anim++;
				if (this->_anim >= avatar.nbAnimations)
					this->_anim = 0;
			}
		} else if (type == "emote") {
			auto emote = this->emotesByName[reward["name"]];
			unsigned offset = 20;

			emote->sprite.setPosition(this->achHolder.behindGear.getPosition() + SokuLib::Vector2u{offset, offset - 2});
			emote->sprite.setSize(this->achHolder.behindGear.getSize() - SokuLib::Vector2u{offset * 2, offset * 2});
			emote->sprite.draw();
		} else if (type == "prop") {

		} else if (type == "accessory") {

		}
	}

	achievement->nameSprite.setPosition(this->achHolder.getText.getPosition() + SokuLib::Vector2i{0, 16});
	this->achHolder.behindGear.setRotation(this->achHolder.behindGear.getRotation() + 0.015);
	if (this->_achTimer >= ACH_GET_TOTAL_TIME) {
		this->_achTimer = 0;
		this->_animCtr = 0;
		this->_anim = 0;
		this->_reward = 0;
		this->achievementAwardQueue.pop_front();
	}
}

void LobbyData::render()
{
	if (this->achievementAwardQueue.empty())
		return;

	SokuLib::SpriteEx sprite;
	auto achievement = this->achievementAwardQueue.front();
	auto reward = achievement->rewards.empty() ? nlohmann::json{} : achievement->rewards[this->_reward];
	auto type = achievement->rewards.empty() ? "title" : reward["type"];
	unsigned offset = 20;

	sprite.render();
	this->achHolder.holder.draw();
	if (type != "title") {
		this->achHolder.behindGear.setPosition(this->achHolder.behindGear.getPosition() + SokuLib::Vector2i{2, 2});
		this->achHolder.behindGear.tint = SokuLib::Color::Black;
		this->achHolder.behindGear.draw();
		this->achHolder.behindGear.setPosition(this->achHolder.behindGear.getPosition() - SokuLib::Vector2i{2, 2});
		this->achHolder.behindGear.tint = SokuLib::Color::White;
		this->achHolder.behindGear.draw();
	}

	this->achHolder.getText.draw();
	achievement->nameSprite.draw();
	if (type == "avatar") {
		auto &avatar = *this->avatarsByCode[reward["id"]];
		auto pos = this->achHolder.behindGear.getPosition();

		pos.x += this->achHolder.behindGear.getSize().x / 2;
		pos.x -= avatar.sprite.rect.width / 2;
		if (this->_achTimer < ACH_GET_FADE_IN_ANIM)
			pos.y = 480 - avatar.sprite.rect.height + static_cast<int>(this->achHolder.behindGear.rect.height * (ACH_GET_FADE_IN_ANIM - this->_achTimer) / ACH_GET_FADE_IN_ANIM);
		else if (this->_achTimer > ACH_GET_TOTAL_TIME - ACH_GET_FADE_OUT_ANIM)
			pos.y = 480 - avatar.sprite.rect.height + static_cast<int>(this->achHolder.behindGear.rect.height * (ACH_GET_FADE_OUT_ANIM - (ACH_GET_TOTAL_TIME - this->_achTimer)) / ACH_GET_FADE_OUT_ANIM);
		else
			pos.y = 480 - avatar.sprite.rect.height;
		avatar.sprite.setPosition(pos + SokuLib::Vector2i{2, 2});
		avatar.sprite.setSize({
			static_cast<unsigned int>(avatar.sprite.rect.width),
			static_cast<unsigned int>(avatar.sprite.rect.height)
		});
		avatar.sprite.setMirroring(false, false);
		avatar.sprite.rect.left = avatar.sprite.rect.width * this->_anim;
		avatar.sprite.rect.top = 0;
		avatar.sprite.tint = SokuLib::Color::Black;
		avatar.sprite.draw();
		avatar.sprite.setPosition(avatar.sprite.getPosition() - SokuLib::Vector2u{2, 2});
		avatar.sprite.tint = SokuLib::Color::White;
		avatar.sprite.draw();
		avatar.sprite.setSize({
			static_cast<unsigned int>(avatar.sprite.rect.width * avatar.scale),
			static_cast<unsigned int>(avatar.sprite.rect.height * avatar.scale)
		});
	} else if (type == "emote") {
		auto emote = this->emotesByName[reward["name"]];

		emote->sprite.setPosition(this->achHolder.behindGear.getPosition() + SokuLib::Vector2u{offset + 2, offset});
		emote->sprite.setSize(this->achHolder.behindGear.getSize() - SokuLib::Vector2u{offset * 2, offset * 2});
		emote->sprite.tint = SokuLib::Color::Black;
		emote->sprite.draw();
		emote->sprite.setPosition(emote->sprite.getPosition() - SokuLib::Vector2u{2, 2});
		emote->sprite.tint = SokuLib::Color::White;
		emote->sprite.draw();
	} else if (type == "prop") {

	} else if (type == "accessory") {

	}
}

void LobbyData::_grantStatsAchievements()
{
	if (achievementsLocked) return;
	// Retain earned rewards; only game-count requirements continue to advance.
	for (auto *achievement : achievementByRequ["play"]) {
		if (!achievement || achievement->awarded) continue;
		const auto &requirement = achievement->requirement;
		if (!requirement.contains("chr") || !requirement.contains("count") ||
			!requirement["chr"].is_number_unsigned() || !requirement["count"].is_number_unsigned()) continue;
		const auto id = requirement["chr"].get<uint64_t>();
		if (id > UINT8_MAX) continue;
		const auto found = characterGames.find(static_cast<uint8_t>(id));
		if (found != characterGames.end() && found->second >= requirement["count"].get<uint64_t>()) {
			achievement->awarded = true; achievementAwardQueue.push_back(achievement);
		}
	}
}

void LobbyData::_grantCrashAchievements()
{
	for (auto &achievement : this->achievementByRequ["crash"])
		if (!achievement->awarded) {
			printf("Crash: grant %s\n", achievement->name.c_str());
			achievement->awarded = true;
			this->achievementAwardQueue.push_back(achievement);
		}
	DeleteFileW(L".crash");
}

void LobbyData::_grantDebugAchievements()
{
	for (auto &achievement : this->achievementByRequ["debug"])
		if (!achievement->awarded) {
			printf("Debug: grant %s\n", achievement->name.c_str());
			achievement->awarded = true;
			this->achievementAwardQueue.push_back(achievement);
		}
}

std::string LobbyData::httpRequest(const std::string &url, const std::string &method, const std::string &data, long timeoutMs, const std::atomic_bool *cancel, const std::string &bearerToken)
{
	if (cancel && !cancel->load()) throw std::runtime_error("HTTP request cancelled");
	long response_code = 0;
	MemoryStruct request_chunk;
	curl_slist *headers = nullptr;
	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), &curl_easy_cleanup);
	if (!handle) throw std::runtime_error("Cannot initialize HTTP request");
	auto *request_handle = handle.get();
	ClientConstructionGuard freeHeaders([&]() noexcept { curl_slist_free_all(headers); });
	auto appendHeader = [&](const char *value) {
		auto *next = curl_slist_append(headers, value);
		if (!next) throw std::bad_alloc();
		headers = next;
	};
	if (method != "GET") {
		curl_easy_setopt(request_handle, CURLOPT_CUSTOMREQUEST, method.c_str());
		appendHeader("Content-Type: application/json");
		if (!bearerToken.empty())
			appendHeader(("Authorization: Bearer " + bearerToken).c_str());
		curl_easy_setopt(request_handle, CURLOPT_HTTPHEADER, headers);
	}
	curl_easy_setopt(request_handle, CURLOPT_WRITEFUNCTION, &LobbyData::writeMemoryCallback);
	curl_easy_setopt(request_handle, CURLOPT_WRITEDATA, (void *)&request_chunk);
	curl_easy_setopt(request_handle, CURLOPT_USERAGENT, ("SokuLobbies " + std::string(modVersion)).c_str());
	curl_easy_setopt(request_handle, CURLOPT_SSL_VERIFYPEER, false);
	curl_easy_setopt(request_handle, CURLOPT_AUTOREFERER, 1L);
	curl_easy_setopt(request_handle, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(request_handle, CURLOPT_TIMEOUT_MS, timeoutMs);
	curl_easy_setopt(request_handle, CURLOPT_NOSIGNAL, 1L);
	if (cancel) {
		curl_easy_setopt(request_handle, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(request_handle, CURLOPT_XFERINFOFUNCTION, +[](void *data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept -> int {
			return !*static_cast<const std::atomic_bool *>(data);
		});
		curl_easy_setopt(request_handle, CURLOPT_XFERINFODATA, cancel);
	}

	curl_easy_setopt(request_handle, CURLOPT_URL, url.c_str());
	if (!data.empty())
		curl_easy_setopt(request_handle, CURLOPT_POSTFIELDS, data.c_str());

	if (cancel && !cancel->load()) throw std::runtime_error("HTTP request cancelled");
	CURLcode res = curl_easy_perform(request_handle);

	if (res != CURLE_OK) {
		throw std::runtime_error(curl_easy_strerror(res));
	}

	curl_easy_getinfo(request_handle, CURLINFO_RESPONSE_CODE, &response_code);
	if (response_code >= 300)
		throw std::invalid_argument("HTTP " + std::to_string(response_code) + ": " + request_chunk.body);
	return std::move(request_chunk.body);
}
