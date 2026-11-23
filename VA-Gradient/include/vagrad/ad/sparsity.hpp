#pragma once
#include "vagrad/ad/plan.hpp"
#include <cstdint>

namespace vagrad::ad {
using LaneMask=std::uint64_t;
// 某 Value 可能依赖哪些种子。它是结构上的保守上界，不是当前数值非零模式。
struct Sparsity {
    std::vector<LaneMask> values;
    std::size_t lanes=0,dense_slots=0,sparse_slots=0;
    std::vector<Diagnostic> diagnostics;
    bool has_errors() const {return !diagnostics.empty();}
};
Sparsity analyze_sparsity(const mir::MirFunction&,const Plan&,const hir::ParamEnv&);
void print_sparsity(std::ostream&,const Sparsity&,const Plan&);
} // namespace vagrad::ad
