#pragma once
#include "vagrad/ir/mir/mir.hpp"

namespace vagrad::mir {
struct DominanceInfo {
    CfgInfo cfg;
    std::vector<std::vector<bool>> dominators; // dominators[b][a]：a 支配 b
    std::vector<std::optional<BlockId>> immediate_dominator;
    std::vector<std::vector<BlockId>> tree_children,frontier;
    std::vector<Diagnostic> diagnostics;
    bool has_errors() const;
    bool dominates(BlockId a,BlockId b) const;
};
DominanceInfo analyze_dominance(const MirFunction&);
void print_dominance(std::ostream&,const DominanceInfo&);
} // namespace vagrad::mir
