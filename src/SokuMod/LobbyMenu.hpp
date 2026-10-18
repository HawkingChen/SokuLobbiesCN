//
// Created by PinkySmile on 02/10/2022.
//

#ifndef SOKULOBBIES_LOBBYMENU_HPP
#define SOKULOBBIES_LOBBYMENU_HPP


#include <atomic>
#include <mutex>
#include <thread>
#include <deque>
#include <unordered_map>
#include <SokuLib.hpp>
#include "Player.hpp"
#include "Socket.hpp"

class Connection;
class LobbyMenu : public SokuLib::IMenu {
private:
	struct Entry {
		std::shared_ptr<Connection> c;
		std::string ip;
		unsigned short port;
		std::string lastName;
		std::pair<unsigned, unsigned> lastPlayerCount = {0, 0};
		SokuLib::DrawUtils::Sprite name;
		SokuLib::DrawUtils::Sprite playerCount;
		bool first = true;
		bool passwd = false;
		std::string redirectIpForLocalServer;

	public:
		Entry(const std::shared_ptr<Connection>& connection, const std::string& ip, unsigned short port);
	};
	struct AvatarShowcase {
		unsigned char action = 0;
		unsigned animCtr = 0;
		unsigned anim = 0;
		bool side = false;
	};

	Player _loadedSettings;
	Player _connectionSettings;
	std::mutex _settingsMutex;
	std::mutex _connectionsMutex;
	SokuLib::MenuConnect *_parent;
	int _customCursor = 0;
	int _lobbyCtr = 0;
	unsigned _avatarTop = 0;
	unsigned _menuCursor = 0;
	std::string _lastError;
	Socket _mainServer;
	std::vector<std::shared_ptr<Entry>> _connections;
	std::vector<std::shared_ptr<Entry>> _retiredEntries;
	unsigned char _menuState = 0;
	std::atomic_bool _open = true;
	bool _active = true;
	std::thread _netThread;
	std::thread _connectThread;
	std::thread _masterThread;
	std::thread _getIpv6Thread;
	SokuLib::DrawUtils::Sprite _ui;
	SokuLib::DrawUtils::Sprite _title;
	SokuLib::DrawUtils::Sprite _hidden;
	SokuLib::DrawUtils::Sprite _version;
	SokuLib::DrawUtils::Sprite _customizeTexts[3];
	SokuLib::DrawUtils::Sprite _customizeSeat;
	SokuLib::DrawUtils::Sprite _playerName;
	SokuLib::DrawUtils::Sprite _customAvatarName;
	SokuLib::DrawUtils::Sprite _customAvatarRequ;
	SokuLib::DrawUtils::Sprite _loadingText;
	SokuLib::DrawUtils::Sprite _loadingHint;
	SokuLib::DrawUtils::Sprite _messageBox;
	SokuLib::DrawUtils::Sprite _loadingGear;
	SokuLib::DrawUtils::Sprite _unlock;
	SokuLib::DrawUtils::Sprite _lock;
	std::vector<AvatarShowcase> _showcases;
	std::mutex _queueMutex;
	std::deque<std::function<void ()>> _workerQueue;
	std::unordered_map<const void *, std::function<void()>> _refreshQueue;
	std::deque<const void *> _refreshOrder;
	void _queueUi(std::function<void()> work, const void *refreshKey = nullptr);

	void _netLoop();
	void _masterServerLoop();
	void _connectLoop();
	void _updateTopAvatarOffset();
	void _refreshAvatarCustomText();
	void _renderAvatarCustomText();
	void _renderCustomAvatarPreview();

	static bool (LobbyMenu::* const _updateCallbacks[])();
	static void (LobbyMenu::* const _renderCallbacks[])();

	bool _normalMenuUpdate();
	bool _joinLobbyUpdate();
	bool _customizeAvatarUpdate();

	void _dummyRender();
	void _customizeAvatarRender();

public:
	LobbyMenu(SokuLib::MenuConnect *parent);
	~LobbyMenu();
	void execUiCallbacks();
	void _() override;
	int onProcess() override;
	int onRender() override;
	void setActive();
	void _refreshName();
	void onHostPrefChanged();
	void onLanguageChanged();
};

#endif //SOKULOBBIES_LOBBYMENU_HPP
