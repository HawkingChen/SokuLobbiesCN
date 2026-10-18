#ifndef SOKULOBBIES_CLIENT_CHAT_LOG_HPP
#define SOKULOBBIES_CLIENT_CHAT_LOG_HPP
#include <filesystem>
#include <string>

// Enqueues owned text only; filesystem operations run on a dedicated worker.
void queueChatLog(std::filesystem::path directory, std::string text);
#endif
