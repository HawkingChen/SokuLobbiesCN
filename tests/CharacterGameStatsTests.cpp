#include "CharacterGameStats.hpp"
#include <fstream>
#include <iostream>
#include <limits>

static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static CharacterGameStats::Counts parse(const std::string &data) {
	std::istringstream input(data, std::ios::binary | std::ios::in); return CharacterGameStats::load(input);
}
static void rejected(const std::string &data) {
	try { parse(data); } catch (const std::invalid_argument &) { return; }
	throw std::runtime_error("Corrupt statistics were accepted");
}
int main(int argc, char **argv) {
	try {
		if (argc == 2) {
			std::ifstream input(argv[1], std::ios::binary);
			const auto games = CharacterGameStats::load(input);
			std::cout << "Valid file: " << games.size() << " characters\n";
			for (const auto &entry : games) std::cout << unsigned(entry.first) << ": " << entry.second << '\n';
			return 0;
		}
		using namespace CharacterGameStats;
		std::ostringstream legacy(std::ios::binary | std::ios::out);
		write(legacy, LEGACY_MAGIC - 192, 4);
		write(legacy, 2, 4); // Two characters; totals are 10 and 6.
		write(legacy, 0, 1); write(legacy, 3, 4); write(legacy, 7, 4); write(legacy, 9, 4); write(legacy, 11, 4);
		write(legacy, 1, 1); write(legacy, 2, 4); write(legacy, 4, 4); write(legacy, 0, 4); write(legacy, 0, 4);
		write(legacy, 1, 4); write(legacy, 0, 1); write(legacy, 1, 4); write(legacy, 10, 4);
		write(legacy, 123, 4); write(legacy, 4, 4); write(legacy, 2, 4); write(legacy, 1, 4);
		write(legacy, 1, 4); write(legacy, 0, 1); write(legacy, 1, 1); write(legacy, 3, 4); write(legacy, 7, 4);
		auto counts = parse(legacy.str());
		require(counts.size() == 2 && counts.at(0) == 10 && counts.at(1) == 6, "Legacy totals not preserved");
		counts[2] = uint64_t{1} << 40; counts[255] = std::numeric_limits<uint64_t>::max();
		std::ostringstream current(std::ios::binary | std::ios::out); save(current, counts);
		require(parse(current.str()) == counts, "64-bit count roundtrip failed");
		require(current.str().size() == 12 + counts.size() * 9, "Removed stats persisted in new format");
		auto corrupt = current.str(); corrupt.back() ^= 1; rejected(corrupt);
		rejected(current.str().substr(0, current.str().size() - 1));
		rejected(current.str() + "unexpected"); rejected(legacy.str().substr(0, legacy.str().size() - 1));
		corrupt = legacy.str(); corrupt[0] ^= 1; rejected(corrupt);
		std::ostringstream duplicate;
		write(duplicate, MAGIC, 4); write(duplicate, 2, 4);
		write(duplicate, 7, 1); write(duplicate, 8, 8); write(duplicate, 7, 1); write(duplicate, 9, 8);
		rejected(duplicate.str());
		std::ostringstream excessive; write(excessive, MAGIC, 4); write(excessive, 257, 4); rejected(excessive.str());
		std::ostringstream empty; save(empty, {}); require(parse(empty.str()).empty(), "Empty count roundtrip failed");
		std::cout << "PASS legacy migration, counts-only format, 64-bit totals and corrupt-file rejection\n";
		return 0;
	} catch (const std::exception &error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
