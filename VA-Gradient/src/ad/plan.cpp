#include "vagrad/ad/plan.hpp"
#include <algorithm>
#include <set>

namespace vagrad::ad {
Plan make_plan(const mir::MirFunction& f,const std::vector<std::string>& names,ParameterMode mode) {
    Plan p;p.hir=f.hir;p.mode=mode;p.diagnostics=mir::verify_mir(f);
    if(p.has_errors()) return p;
    auto fail=[&](std::string message){p.diagnostics.push_back({Severity::Error,"AD_SEED",std::move(message),{},{}});};
    if(f.form!=mir::MirForm::Ssa) fail("AD requires verified SSA");
    if(names.empty()||names.size()>64) fail("choose 1..64 independent Real parameter seeds");
    std::set<std::string> seen;
    for(const auto& name:names) {
        const auto found=std::find_if(f.hir->parameters.begin(),f.hir->parameters.end(),[&](const auto& q){
            const auto& symbol=f.hir->bindings->symbol(q.symbol);
            return symbol.scope==f.hir->scope&&symbol.name==name;});
        if(found==f.hir->parameters.end()||!seen.insert(name).second) {fail("unknown or duplicate seed: "+name);continue;}
        if(found->type!=hir::Type::Real||!found->overridable) {fail("seed must be an overridable Real parameter: "+name);continue;}
        p.parameters.push_back(found->symbol);
    }
    return p;
}
Jet default_parameter_jet(const Plan&,const hir::ParamEnv&,hir::SymbolId);
Jet parameter_jet(const Plan& p,const hir::ParamEnv& env,hir::SymbolId id) {
    if(p.has_errors()||!p.hir||env.has_errors()||env.bindings!=p.hir->bindings||
       id.value>=env.values.size()||!env.values[id.value]||
       !hir::numeric(hir::scalar_type(*env.values[id.value]))) throw Error("invalid AD effective parameter environment");
    if(p.mode==ParameterMode::Defaults) return default_parameter_jet(p,env,id);
    const auto lane=std::find(p.parameters.begin(),p.parameters.end(),id);
    const auto value=hir::number(*env.values[id.value]);
    return lane==p.parameters.end()?constant(value,p.parameters.size()):
        seed(value,p.parameters.size(),static_cast<std::size_t>(lane-p.parameters.begin()));
}
} // namespace vagrad::ad
