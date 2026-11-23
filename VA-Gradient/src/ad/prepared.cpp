#include "vagrad/ad/prepared.hpp"
#include <algorithm>

namespace vagrad::ad {
namespace {
std::vector<hir::SymbolId> frozen_overrides(const Plan& p,const hir::ParamEnv& env) {
    std::vector<hir::SymbolId> result;
    if(p.mode==ParameterMode::Defaults) for(const auto id:env.overridden)
        if(std::find(p.parameters.begin(),p.parameters.end(),id)==p.parameters.end()) result.push_back(id);
    std::sort(result.begin(),result.end(),[](const auto a,const auto b){return a.value<b.value;});
    result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}
}
Prepared prepare(const mir::MirFunction& f,const hir::ParamEnv& env,const std::vector<std::string>& seeds,ParameterMode mode) {
    Prepared p;p.executable_=std::make_shared<const mir::Executable>(f);p.seeds_=seeds;
    p.plan_=make_plan(f,seeds,mode);p.diagnostics_=p.plan_.diagnostics;
    if(p.has_errors()) return p;
    p.diagnostics_=hir::validate_effective_parameters(*f.hir,env);
    if(p.has_errors()) return p;
    p.derivatives_=lower_derivatives(f);p.diagnostics_=p.derivatives_.diagnostics;
    if(p.has_errors()) return p;
    p.sparsity_=analyze_sparsity(f,p.plan_,env);p.diagnostics_=p.sparsity_.diagnostics;
    p.frozen_overrides_=frozen_overrides(p.plan_,env);return p;
}
bool Prepared::accepts(const hir::ParamEnv& env) const {
    return !has_errors()&&!env.has_errors()&&env.bindings==function().hir->bindings&&
        env.values.size()==function().hir->bindings->symbols.size()&&
        frozen_overrides(plan_,env)==frozen_overrides_;
}
} // namespace vagrad::ad
