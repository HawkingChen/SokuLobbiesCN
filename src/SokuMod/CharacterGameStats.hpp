#ifndef SOKULOBBIES_CHARACTER_GAME_STATS_HPP
#define SOKULOBBIES_CHARACTER_GAME_STATS_HPP
#include <cstdint>
#include <istream>
#include <ostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace CharacterGameStats {
using Counts = std::map<uint8_t, uint64_t>;
constexpr uint32_t MAGIC = 0x31474353; // SCG1: character game counts only.
constexpr uint32_t LEGACY_MAGIC = 0xF2A6E790;
inline uint64_t read(std::istream &input, unsigned bytes) {
	uint64_t value = 0;
	for (unsigned i = 0; i < bytes; ++i) {
		const auto byte = input.get();
		if (byte == std::char_traits<char>::eof()) throw std::invalid_argument("Truncated statistics file");
		value |= uint64_t(static_cast<uint8_t>(byte)) << (i * 8);
	}
	return value;
}
inline void write(std::ostream &output, uint64_t value, unsigned bytes) {
	for (unsigned i = 0; i < bytes; ++i) output.put(static_cast<char>(value >> (i * 8)));
}
inline uint32_t hash(const std::string &data) {
	uint32_t value = 2166136261u;
	for (auto byte : data) { value ^= static_cast<uint8_t>(byte); value *= 16777619u; }
	return value;
}
inline uint32_t count(std::istream &input, uint32_t maximum) {
	const auto value = static_cast<uint32_t>(read(input, 4));
	if (value > maximum) throw std::invalid_argument("Invalid statistics entry count");
	return value;
}
inline Counts load(std::istream &input) {
	const auto magic = static_cast<uint32_t>(read(input, 4));
	Counts result;
	if (magic == MAGIC) {
		std::ostringstream payload(std::ios::binary | std::ios::out);
		const auto entries = count(input, 256); write(payload, entries, 4);
		for (uint32_t i = 0; i < entries; ++i) {
			const auto id = static_cast<uint8_t>(read(input, 1));
			const auto games = read(input, 8);
			if (!result.emplace(id, games).second) throw std::invalid_argument("Duplicate character count");
			write(payload, id, 1); write(payload, games, 8);
		}
		if (read(input, 4) != hash(payload.str())) throw std::invalid_argument("Statistics checksum mismatch");
	} else {
		// Import the old file without retaining or using card/matchup statistics.
		uint32_t expected = LEGACY_MAGIC;
		auto subtract = [&](unsigned bytes) { const auto value = static_cast<uint32_t>(read(input, bytes)); expected -= value; return value; };
		const auto characters = count(input, 256); expected -= characters;
		for (uint32_t i = 0; i < characters; ++i) {
			const auto id = static_cast<uint8_t>(subtract(1));
			const uint64_t losses = subtract(4), wins = subtract(4);
			subtract(4); subtract(4);
			if (!result.emplace(id, wins + losses).second) throw std::invalid_argument("Duplicate legacy character");
		}
		std::set<uint32_t> ids;
		const auto cardCharacters = count(input, 256); expected -= cardCharacters;
		for (uint32_t i = 0; i < cardCharacters; ++i) {
			if (!ids.insert(subtract(1)).second) throw std::invalid_argument("Duplicate legacy card character");
			const auto cards = count(input, 4096); subtract(4);
			std::set<uint32_t> cardIds;
			for (uint32_t j = 0; j < cards; ++j) {
				if (!cardIds.insert(subtract(4)).second) throw std::invalid_argument("Duplicate legacy card");
				subtract(4); subtract(4); subtract(4);
			}
		}
		ids.clear();
		const auto matchups = count(input, 65536); expected -= matchups;
		for (uint32_t i = 0; i < matchups; ++i) {
			const auto left = subtract(1), right = subtract(1);
			if (!ids.insert(left * 256 + right).second) throw std::invalid_argument("Duplicate legacy matchup");
			subtract(4); subtract(4);
		}
		if (magic != expected) throw std::invalid_argument("Invalid legacy statistics checksum");
	}
	if (input.peek() != std::char_traits<char>::eof()) throw std::invalid_argument("Unexpected statistics trailing data");
	return result;
}
inline void save(std::ostream &output, const Counts &counts) {
	std::ostringstream payload(std::ios::binary | std::ios::out);
	write(payload, counts.size(), 4);
	for (const auto &entry : counts) { write(payload, entry.first, 1); write(payload, entry.second, 8); }
	write(output, MAGIC, 4);
	const auto data = payload.str(); output.write(data.data(), data.size()); write(output, hash(data), 4);
}
}
#endif
