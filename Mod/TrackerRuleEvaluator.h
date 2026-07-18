#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace bloodstained::tracker {

enum class RuleOperation : std::uint8_t {
    NEVER,
    ALWAYS,
    HAS_ALL,
    HAS_ANY,
    AND,
    OR,
    COUNT,
    COUNT_UNIQUE,
};

struct ItemRequirement {
    std::uint32_t item;
    std::uint32_t count;
};

struct RuleNode {
    RuleOperation operation;
    std::uint32_t operand_offset;
    std::uint32_t operand_count;
    std::uint32_t argument;
};

struct NamedRule {
    std::string_view name;
    std::uint32_t node;
};

class RuleEvaluator {
   public:
    RuleEvaluator(std::span<const ItemRequirement> requirements, std::span<const std::uint32_t> operands,
                  std::span<const RuleNode> nodes);

    bool Evaluate(std::uint32_t node, std::span<const std::uint32_t> inventory) const;

   private:
    std::span<const ItemRequirement> requirements_;
    std::span<const std::uint32_t> operands_;
    std::span<const RuleNode> nodes_;
};

}  // namespace bloodstained::tracker
