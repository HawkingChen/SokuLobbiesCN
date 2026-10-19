#ifndef SOKULOBBIES_UTF8_NAME_HPP
#define SOKULOBBIES_UTF8_NAME_HPP

#include <string>
#include <string_view>
#include <vector>

namespace ClientNames {
// Return the byte length of a scalar, zero for invalid input, or -1 for a
// potentially valid scalar cut off at the end of a fixed-size wire field.
inline int scalarLength(std::string_view text, size_t offset) noexcept {
	const auto first = static_cast<unsigned char>(text[offset]);
	if (first < 0x80) return 1;
	const int length = first >= 0xC2 && first <= 0xDF ? 2 :
		first >= 0xE0 && first <= 0xEF ? 3 : first >= 0xF0 && first <= 0xF4 ? 4 : 0;
	if (!length) return 0;
	for (int i = 1; i < length; ++i) {
		if (offset + i >= text.size()) return -1;
		const auto next = static_cast<unsigned char>(text[offset + i]);
		if (next < 0x80 || next > 0xBF) return 0;
		if (i == 1 && ((first == 0xE0 && next < 0xA0) || (first == 0xED && next > 0x9F) ||
			(first == 0xF0 && next < 0x90) || (first == 0xF4 && next > 0x8F))) return 0;
	}
	return length;
}

inline bool validRatingName(std::string_view text) noexcept {
	if (text.empty()) return false;
	for (size_t i = 0; i < text.size();) {
		if (text[i] == '\0') return false;
		const int length = scalarLength(text, i);
		if (length <= 0) return false;
		i += length;
	}
	return true;
}

// Presentation only. Never use this repaired value as an identity, cache key,
// blacklist key or ratings query: lost bytes cannot be reconstructed.
inline std::string displayName(std::string_view text) {
	std::string output;
	output.reserve(text.size());
	for (size_t i = 0; i < text.size();) {
		const int length = scalarLength(text, i);
		if (length < 0) break;
		if (!length || text[i] == '\0') { output += "\xEF\xBF\xBD"; ++i; }
		else { output.append(text.data() + i, length); i += length; }
	}
	return output;
}

inline std::vector<std::string> ratingNames(const std::vector<std::string> &names) {
	std::vector<std::string> valid;
	valid.reserve(names.size());
	for (const auto &name : names) if (validRatingName(name)) valid.push_back(name);
	return valid;
}
}

#endif
