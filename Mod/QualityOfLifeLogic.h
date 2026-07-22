#pragma once

#include <algorithm>

namespace bloodstained::qol {

constexpr int MAX_SHARD_GRADE = 9;

constexpr int ExcessShardQuantity(int currentGrade, int incomingQuantity) {
    if (incomingQuantity <= 0) return 0;
    return std::max(0, currentGrade + incomingQuantity - MAX_SHARD_GRADE);
}

}  // namespace bloodstained::qol
