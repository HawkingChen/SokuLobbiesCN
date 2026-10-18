#ifndef SOKULOBBIES_STATSMENU_HPP
#define SOKULOBBIES_STATSMENU_HPP
#include <memory>
#include <vector>
#include <SokuLib.hpp>

class StatsMenu : public SokuLib::IMenu {
	struct Entry {
		SokuLib::DrawUtils::Sprite portrait, name, games;
	};
	SokuLib::DrawUtils::Sprite _title, _nameHeader, _gamesHeader, _help;
	std::vector<std::unique_ptr<Entry>> _entries;
	size_t _start = 0;
	static constexpr size_t PAGE_SIZE = 10;
	void _positionEntries();
public:
	StatsMenu();
	void _() override;
	int onProcess() override;
	int onRender() override;
};
#endif
