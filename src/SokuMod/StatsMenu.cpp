#include <filesystem>
#include <algorithm>
#include <encodingConverter.hpp>
#include <../directx/dinput.h>
#include "StatsMenu.hpp"
#include "LobbyData.hpp"
#include "data.hpp"
#include "createUTFTexture.hpp"

namespace {
	void text(SokuLib::DrawUtils::Sprite &sprite, const std::wstring &value, unsigned fontSize, SokuLib::Vector2i position) {
		int handle = 0;
		SokuLib::Vector2i size{};
		// Keep glyph bearings and the shadow away from the texture's clipped edge.
		constexpr int padding = 2;
		auto &font = lobbyData->getFont(fontSize);
		const auto offsetX = font.description.offsetX;
		const auto offsetY = font.description.offsetY;
		font.description.offsetX = padding;
		font.description.offsetY = padding;
		const auto created = createTextTexture(handle, value.c_str(), font, {560, 32}, &size);
		font.description.offsetX = offsetX;
		font.description.offsetY = offsetY;
		if (!created) return;
		size.x = (std::min)(560, size.x + padding * 2);
		size.y = (std::min)(32, size.y + padding * 2);
		sprite.texture.setHandle(handle, {560, 32});
		sprite.setSize(size.to<unsigned>());
		sprite.rect = {0, 0, size.x, size.y};
		sprite.setPosition({position.x - padding, position.y - padding});
	}
	bool repeat(int value) {
		const auto amount = std::abs(value);
		return amount == 1 || (amount > 36 && amount % 6 == 0);
	}
}

StatsMenu::StatsMenu() {
	text(_title, chineseLanguage ? L"角色局数" : L"Character Games", 20, {40, 65});
	text(_nameHeader, chineseLanguage ? L"角色" : L"Character", 14, {150, 110});
	text(_gamesHeader, chineseLanguage ? L"局数" : L"Games", 14, {460, 110});
	text(_help, chineseLanguage ? L"上下：滚动    左右：翻页    B / ESC：返回" :
		L"Up/Down: Scroll   Left/Right: Page   B / ESC: Back", 12, {40, 440});
	for (const auto &character : characters) {
		auto entry = std::make_unique<Entry>();
		if (character.first == SokuLib::CHARACTER_RANDOM)
			entry->portrait.texture.loadFromFile((std::filesystem::path(profileFolderPath) / "assets/menu/random_face000.png").string().c_str());
		else
			entry->portrait.texture.loadFromGame(("data/character/" + character.second.codeName + "/face/face000.png").c_str());
		const auto size = entry->portrait.texture.getSize();
		entry->portrait.setSize({size.x / 2, size.y / 2});
		entry->portrait.rect.width = size.x; entry->portrait.rect.height = size.y;
		text(entry->name, convertEncoding<char, wchar_t, UTF8Decode, UTF16Encode>(character.second.fullName), 12, {150, 0});
		const auto found = lobbyData->characterGames.find(static_cast<uint8_t>(character.first));
		text(entry->games, std::to_wstring(found == lobbyData->characterGames.end() ? 0 : found->second), 12, {460, 0});
		_entries.push_back(std::move(entry));
	}
	_positionEntries();
}

void StatsMenu::_positionEntries() {
	for (size_t i = _start; i < _entries.size() && i - _start < PAGE_SIZE; ++i) {
		const auto y = static_cast<int>(140 + 27 * (i - _start));
		_entries[i]->portrait.setPosition({30, y - 4});
		_entries[i]->name.setPosition({150, y});
		_entries[i]->games.setPosition({460, y});
	}
}

void StatsMenu::_() { *(int *)0x882a94 = 0x16; }

int StatsMenu::onProcess() {
	const auto &input = SokuLib::inputMgrs.input;
	if (SokuLib::checkKeyOneshot(DIK_ESCAPE, 0, 0, 0) || input.b == 1) {
		playSound(0x29); return false;
	}
	const auto maximum = _entries.size() > PAGE_SIZE ? _entries.size() - PAGE_SIZE : 0;
	const auto before = _start;
	if (repeat(input.verticalAxis)) {
		if (input.verticalAxis < 0 && _start) --_start;
		else if (input.verticalAxis > 0 && _start < maximum) ++_start;
	}
	if (repeat(input.horizontalAxis)) {
		if (input.horizontalAxis < 0) _start = _start > PAGE_SIZE ? _start - PAGE_SIZE : 0;
		else _start = (std::min)(maximum, _start + PAGE_SIZE);
	}
	if (before != _start) { playSound(0x27); _positionEntries(); }
	return true;
}

int StatsMenu::onRender() {
	_title.draw(); _nameHeader.draw(); _gamesHeader.draw(); _help.draw();
	for (size_t i = _start; i < _entries.size() && i - _start < PAGE_SIZE; ++i) {
		_entries[i]->portrait.draw(); _entries[i]->name.draw(); _entries[i]->games.draw();
	}
	return 0;
}
