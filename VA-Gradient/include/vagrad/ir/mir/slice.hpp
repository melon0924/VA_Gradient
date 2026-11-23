#pragma once
#include "vagrad/ir/mir/mir.hpp"

namespace vagrad::mir {
enum class SliceEffects { Preserve, TargetOnly };
struct SliceInputs {
    std::vector<hir::SymbolId> parameters,nodes;
    std::vector<hir::Probe> potentials;
    bool temperature=false;
};
struct EffectDecision { BlockId block;std::size_t instruction;Opcode opcode;hir::Origin origin;bool retained; };
struct SliceResult {
    explicit SliceResult(MirFunction source):function(std::move(source)) {}
    MirFunction function;
    std::string target;
    SliceEffects effects=SliceEffects::Preserve;
    SliceInputs target_inputs,execution_inputs;
    std::vector<EffectDecision> effect_decisions;
    std::size_t original_instructions=0,retained_instructions=0;
    bool cfg_has_cycle=false,conservative_control=false;
    std::vector<Diagnostic> diagnostics;
    bool has_errors() const;
};
SliceResult slice_ssa(const MirFunction&,std::string target,SliceEffects effects=SliceEffects::Preserve);
void print_slice(std::ostream&,const SliceResult&);
} // namespace vagrad::mir
