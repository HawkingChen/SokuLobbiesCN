#include "ClientChatLog.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
	class ChatLogWriter {
		struct Record { std::filesystem::path directory; std::string text; std::chrono::system_clock::time_point received; };
		std::mutex mutex;
		std::condition_variable ready;
		std::deque<Record> records;
		size_t bytes = 0;
		bool stopping = false;
		std::chrono::steady_clock::time_point stopDeadline{};
		std::thread worker;
		void run() {
			while (true) {
				Record record;
				{
					std::unique_lock<std::mutex> lock(mutex);
					ready.wait(lock, [&] { return stopping || !records.empty(); });
					if (records.empty() || (stopping && std::chrono::steady_clock::now() >= stopDeadline)) return;
					record = std::move(records.front()); records.pop_front(); bytes -= record.text.size();
				}
				try {
					std::filesystem::create_directories(record.directory);
					const auto time = std::chrono::system_clock::to_time_t(record.received);
					std::tm local{};
#ifdef _WIN32
					localtime_s(&local, &time);
#else
					localtime_r(&time, &local);
#endif
					std::ostringstream filename; filename << std::put_time(&local, "%Y-%m-%d") << ".txt";
					std::ofstream output(record.directory / filename.str(), std::ios::app);
					output << std::put_time(&local, "%H:%M:%S") << ' ' << record.text << '\n';
				} catch (...) {}
			}
		}
	public:
		ChatLogWriter() : worker([this] { run(); }) {}
		~ChatLogWriter() {
			{ std::lock_guard<std::mutex> lock(mutex); stopping = true; stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1); }
			ready.notify_all(); worker.join();
		}
		void push(std::filesystem::path directory, std::string text) {
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping || records.size() >= 2048 || text.size() > 16384 || bytes + text.size() > 1024 * 1024) return;
			for (auto &ch : text) if (ch == '\n' || ch == '\r') ch = ' ';
			bytes += text.size(); records.push_back({std::move(directory), std::move(text), std::chrono::system_clock::now()});
			ready.notify_one();
		}
	};
}

void queueChatLog(std::filesystem::path directory, std::string text) {
	static ChatLogWriter writer;
	writer.push(std::move(directory), std::move(text));
}
