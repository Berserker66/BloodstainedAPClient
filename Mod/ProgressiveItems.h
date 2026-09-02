#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

#include "TrackerData.generated.h"

namespace bloodstained::items {

inline const tracker::generated::ProgressiveItemBinding* FindProgressiveItem(std::uint64_t itemId) {
    const auto& bindings = tracker::generated::PROGRESSIVE_ITEM_BINDINGS;
    const auto binding = std::lower_bound(
        bindings.begin(), bindings.end(), itemId,
        [](const auto& entry, std::uint64_t id) { return entry.id < id; });
    return binding == bindings.end() || binding->id != itemId ? nullptr : &*binding;
}

inline std::optional<std::string_view> ResolveProgressiveItem(
    std::uint64_t itemId, std::uint32_t previouslyAwardedOccurrences) {
    const auto* binding = FindProgressiveItem(itemId);
    if (!binding || binding->level_count == 0) return std::nullopt;

    const auto level = std::min<std::uint32_t>(previouslyAwardedOccurrences, binding->level_count - 1);
    const auto index = static_cast<std::size_t>(binding->level_offset) + level;
    if (index >= tracker::generated::PROGRESSIVE_ITEM_LEVELS.size()) return std::nullopt;
    return tracker::generated::PROGRESSIVE_ITEM_LEVELS[index];
}

}  // namespace bloodstained::items
