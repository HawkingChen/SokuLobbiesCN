#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace NearbyRatingCards {
constexpr uint32_t NO_TARGET = (std::numeric_limits<uint32_t>::max)();

// A second card belongs to the same overlapping group as the automatic target,
// never to an arbitrary second-nearest player or a manually selected F8 target.
template<class Players>
uint32_t secondTarget(const Players &players, uint32_t self, uint32_t selfX, uint32_t selfY,
	uint32_t primaryId, bool manual)
{
	if (manual || primaryId == NO_TARGET) return NO_TARGET;
	const auto primary = std::find_if(players.begin(), players.end(),
		[&](const auto &player) { return player.id == primaryId; });
	if (primary == players.end()) return NO_TARGET;
	uint32_t result = NO_TARGET;
	int64_t best = (std::numeric_limits<int64_t>::max)();
	for (const auto &player : players) {
		if (!player.id || player.id == NO_TARGET || player.id == self || player.id == primaryId) continue;
		const int64_t dx = static_cast<int64_t>(player.pos.x) - primary->pos.x;
		const int64_t dy = static_cast<int64_t>(player.pos.y) - primary->pos.y;
		if (dx < -32 || dx > 32 || dy < -24 || dy > 24) continue;
		const int64_t fromSelfX = static_cast<int64_t>(player.pos.x) - selfX;
		const int64_t fromSelfY = static_cast<int64_t>(player.pos.y) - selfY;
		if (fromSelfX < -450 || fromSelfX > 450 || fromSelfY < -450 || fromSelfY > 450 ||
			fromSelfX * fromSelfX + fromSelfY * fromSelfY > 450 * 450) continue;
		const auto distance = dx * dx + dy * dy;
		if (distance < best || (distance == best && player.id < result)) {
			best = distance; result = player.id;
		}
	}
	return result;
}

struct Size { int width, height; };
struct Position { int x, y, secondY; };

inline Position stackPosition(Size first, Size second, int anchorX, int anchorY,
	unsigned mode, int offsetX, int offsetY)
{
	constexpr int gap = 6;
	const int width = (std::max)(first.width, second.width);
	const int height = first.height + gap + second.height;
	int x = anchorX - width / 2 + offsetX;
	int y = anchorY - first.height - 52 + offsetY;
	if (mode == 1 || mode == 2) y = anchorY - first.height / 2 + offsetY;
	if (mode == 2) {
		x = anchorX + 40 + offsetX;
		if (x + width > 636) x = anchorX - width - 40 + offsetX;
	}
	x = (std::clamp)(x, 4, 636 - width);
	y = (std::clamp)(y, 4, 476 - height);
	return {x, y, y + first.height + gap};
}
}
