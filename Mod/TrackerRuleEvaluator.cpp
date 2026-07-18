#include "TrackerRuleEvaluator.h"

namespace bloodstained::tracker {

RuleEvaluator::RuleEvaluator(std::span<const ItemRequirement> requirements, std::span<const std::uint32_t> operands,
                             std::span<const RuleNode> nodes)
    : requirements_(requirements), operands_(operands), nodes_(nodes) {}

bool RuleEvaluator::Evaluate(std::uint32_t nodeIndex, std::span<const std::uint32_t> inventory) const {
    const RuleNode& node = nodes_[nodeIndex];
    switch (node.operation) {
        case RuleOperation::NEVER:
            return false;
        case RuleOperation::ALWAYS:
            return true;
        case RuleOperation::HAS_ALL:
            for (std::uint32_t i = 0; i < node.operand_count; ++i) {
                const ItemRequirement& requirement = requirements_[node.operand_offset + i];
                if (inventory[requirement.item] < requirement.count) return false;
            }
            return true;
        case RuleOperation::HAS_ANY:
            for (std::uint32_t i = 0; i < node.operand_count; ++i) {
                const ItemRequirement& requirement = requirements_[node.operand_offset + i];
                if (inventory[requirement.item] >= requirement.count) return true;
            }
            return false;
        case RuleOperation::AND:
            for (std::uint32_t i = 0; i < node.operand_count; ++i) {
                if (!Evaluate(operands_[node.operand_offset + i], inventory)) return false;
            }
            return true;
        case RuleOperation::OR:
            for (std::uint32_t i = 0; i < node.operand_count; ++i) {
                if (Evaluate(operands_[node.operand_offset + i], inventory)) return true;
            }
            return false;
        case RuleOperation::COUNT: {
            std::uint64_t found = 0;
            for (std::uint32_t i = 0; i < node.operand_count; ++i) {
                found += inventory[operands_[node.operand_offset + i]];
                if (found >= node.argument) return true;
            }
            return false;
        }
        case RuleOperation::COUNT_UNIQUE: {
            std::uint32_t found = 0;
            for (std::uint32_t i = 0; i < node.operand_count; ++i) {
                if (inventory[operands_[node.operand_offset + i]] > 0) ++found;
                if (found >= node.argument) return true;
            }
            return false;
        }
    }
    return false;
}

}  // namespace bloodstained::tracker
