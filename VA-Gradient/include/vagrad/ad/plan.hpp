#pragma once
#include "vagrad/ad/forward.hpp"
#include "vagrad/ir/mir/eval.hpp"

namespace vagrad::ad {
enum class ParameterMode { Effective, Defaults };
struct ParameterError:Error {
    hir::Origin origin;
    ParameterError(hir::Origin source,std::string message):Error(std::move(message)),origin(source) {}
};
struct Plan {
    std::shared_ptr<const hir::HirModule> hir;
    std::vector<hir::SymbolId> parameters; // 分量顺序与用户指定顺序一致
    std::vector<Diagnostic> diagnostics;
    ParameterMode mode=ParameterMode::Effective;
    bool has_errors() const {return !diagnostics.empty();}
};
Plan make_plan(const mir::MirFunction&,const std::vector<std::string>& seed_names,ParameterMode mode=ParameterMode::Effective);
// Effective: 其他有效参数固定。Defaults: 非覆盖、非种子参数沿默认表达式传播。
Jet parameter_jet(const Plan&,const hir::ParamEnv&,hir::SymbolId);
} // namespace vagrad::ad
