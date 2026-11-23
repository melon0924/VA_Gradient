#include "vagrad/validation/gradcheck.hpp"
#include <algorithm>
#include <cmath>
#include <ostream>

namespace vagrad::validation {
namespace {
namespace H=hir;namespace M=mir;
class Trace final:public M::ExecutionObserver {
public:
    std::vector<std::pair<std::size_t,bool>> branches;
    void branch(const M::Terminator& t,const H::Scalar& c) override {
        branches.push_back({t.condition->value,H::number(c)!=0});
    }
};
}
bool GradientCheck::passed() const {
    return !has_errors()&&!rows.empty()&&std::all_of(rows.begin(),rows.end(),[](const auto& r){return r.passed;});
}
GradientCheck check_gradients(const M::MirFunction& f,const H::ParamEnv& env,const H::EvalInputs& inputs,
    const std::vector<std::string>& names,const GradientOptions& options,ad::ParameterMode mode) {
    GradientCheck r;
    auto fail=[&](std::string code,std::string message) {
        r.diagnostics.push_back({Severity::Error,std::move(code),std::move(message),{},{}});
    };
    if(!std::isfinite(options.relative_step)||options.relative_step<=0||
       !std::isfinite(options.absolute_step)||options.absolute_step<=0||
       !std::isfinite(options.relative_tolerance)||options.relative_tolerance<0||
       !std::isfinite(options.absolute_tolerance)||options.absolute_tolerance<0) {
        fail("GRADCHECK_OPTION","steps must be finite/positive, tolerances finite/nonnegative");return r;
    }
    for(const auto& [name,step]:options.steps)
        if(std::find(names.begin(),names.end(),name)==names.end()||!std::isfinite(step)||step<=0) {
            fail("GRADCHECK_OPTION","invalid or unselected seed step: "+name);return r;
        }
    r.automatic=ad::evaluate(f,env,inputs,names,mode);
    if(r.has_errors()) return r;
    const auto plan=ad::make_plan(f,names,mode);
    Trace baseline;
    const auto base=M::evaluate_mir(f,env,inputs,&baseline);
    if(base.has_errors()) {r.diagnostics=base.diagnostics;return r;}
    for(std::size_t lane=0;lane<names.size();++lane) {
        const auto id=plan.parameters[lane];
        const auto value=H::number(*env.values[id.value]);
        const auto h=options.steps.contains(names[lane])?options.steps.at(names[lane]):
            std::max(std::abs(value)*options.relative_step,options.absolute_step);
        const auto vp=value+h,vm=value-h;
        if(!std::isfinite(vp)||!std::isfinite(vm)||vp==value||vm==value||!std::isfinite(vp-vm)) {
            fail("GRADCHECK_STEP","perturbation must produce distinct finite values for "+names[lane]);return r;
        }
        auto perturb=[&](double selected) {
            if(mode==ad::ParameterMode::Effective) {
                auto e=env;e.values[id.value]=selected;
                e.diagnostics=H::validate_effective_parameters(*f.hir,e);return e;
            }
            std::map<std::string,H::Scalar> overrides;
            for(const auto p:env.overridden)
                overrides[f.hir->bindings->symbol(p).name]=*env.values[p.value];
            // 多种子求偏导：其他种子也独立固定，不能恢复成依赖默认式。
            for(const auto p:plan.parameters)
                overrides[f.hir->bindings->symbol(p).name]=*env.values[p.value];
            overrides[names[lane]]=selected;
            return H::resolve_parameters(*f.hir,overrides);
        };
        const auto ep=perturb(vp),em=perturb(vm);
        if(ep.has_errors()||em.has_errors()) {
            fail("GRADCHECK_RANGE","perturbation violates parameter constraints for "+names[lane]+
                "; choose a smaller --step or an interior point");
            const auto& ds=ep.has_errors()?ep.diagnostics:em.diagnostics;
            r.diagnostics.insert(r.diagnostics.end(),ds.begin(),ds.end());return r;
        }
        Trace tp,tm;
        const auto plus=M::evaluate_mir(f,ep,inputs,&tp),minus=M::evaluate_mir(f,em,inputs,&tm);
        if(plus.has_errors()||minus.has_errors()) {
            fail("GRADCHECK_EVAL","perturbed primal failed for "+names[lane]);
            const auto& ds=plus.has_errors()?plus.diagnostics:minus.diagnostics;
            r.diagnostics.insert(r.diagnostics.end(),ds.begin(),ds.end());return r;
        }
        if(tp.branches!=baseline.branches||tm.branches!=baseline.branches) {
            fail("GRADCHECK_PATH","perturbation changes the executed branch path for "+names[lane]+
                "; reduce --step or choose a point away from a boundary");return r;
        }
        auto row=[&](std::string output,double automatic,double p,double m) {
            const auto numerical=(p-m)/(vp-vm);
            if(!std::isfinite(numerical)) {fail("GRADCHECK_NUMERIC","non-finite difference for "+output);return;}
            const auto error=std::abs(automatic-numerical);
            const auto tolerance=options.absolute_tolerance+options.relative_tolerance*
                std::max(std::abs(automatic),std::abs(numerical));
            if(!std::isfinite(error)||!std::isfinite(tolerance)) {
                fail("GRADCHECK_NUMERIC","non-finite error/tolerance for "+output);return;
            }
            r.rows.push_back({std::move(output),names[lane],automatic,numerical,h,error,tolerance,error<=tolerance});
        };
        for(const auto& [name,gradient]:r.automatic.observations) {
            if(!plus.observations.contains(name)||!minus.observations.contains(name)) {
                fail("GRADCHECK_OUTPUT","perturbed output is not defined: "+name);return r;
            }
            row(name,gradient[lane],H::number(plus.observations.at(name)),H::number(minus.observations.at(name)));
        }
        for(const auto& [name,gradient]:r.automatic.branch_flows)
            row("flow:"+name,gradient[lane],plus.branch_flows.at(name),minus.branch_flows.at(name));
    }
    if(r.rows.empty()) fail("GRADCHECK_OUTPUT","no numeric output to verify");
    return r;
}
void print_gradient_check(std::ostream& out,const GradientCheck& r) {
    out<<"Gradient check status="<<(r.has_errors()?"error":r.passed()?"pass":"fail")<<'\n';
    for(const auto& d:r.diagnostics) out<<d.code<<": "<<d.message<<'\n';
    for(const auto& d:r.automatic.diagnostics) out<<d.code<<": "<<d.message<<'\n';
    for(const auto& d:r.automatic.primal.diagnostics) out<<d.code<<": "<<d.message<<'\n';
    for(const auto& row:r.rows)
        out<<(row.passed?"PASS ":"FAIL ")<<"d("<<row.output<<")/d("<<row.seed<<")"
           <<" AD="<<H::scalar_text(row.automatic)<<" FD="<<H::scalar_text(row.numerical)
           <<" h="<<H::scalar_text(row.step)<<" error="<<H::scalar_text(row.error)
           <<" tolerance="<<H::scalar_text(row.tolerance)<<'\n';
}
} // namespace vagrad::validation
