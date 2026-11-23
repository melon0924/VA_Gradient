#include "vagrad/validation/gradcheck.hpp"
#include "vagrad/ir/mir/slice.hpp"
#include "mir_test_support.hpp"
#include <iostream>

namespace {
using namespace mir_test;namespace V=vagrad::validation;namespace A=vagrad::ad;
M::MirFunction model(std::string parameters,std::string body) {
    return M::promote_to_ssa(text("module m; "+parameters+" real ids; analog "+body+" endmodule"));
}
void model_grid() {
    const auto f=M::promote_to_ssa(sample());
    V::GradientOptions opts;opts.steps={{"G0",1e-7},{"VTH0",1e-5},{"TNOM",.01}};
    const auto r=V::check_gradients(f,H::resolve_parameters(*f.hir),inputs(),{"G0","VTH0","TNOM"},opts);
    if(!r.passed()) {V::print_gradient_check(std::cerr,r);throw std::runtime_error("model gradient matrix");}
    require(r.rows.size()==18,"all output/seed entries checked");
    const auto sliced=M::slice_ssa(f,"ids@exit",M::SliceEffects::TargetOnly);
    auto input=inputs();input.temperature_kelvin.reset();
    require(V::check_gradients(sliced.function,H::resolve_parameters(*f.hir),input,{"G0","VTH0"}).passed(),
        "slice gradient harness uses projected outputs");
}
void parameter_modes() {
    const auto f=model("parameter real A=2; parameter real B=3*A; localparam real C=B+1;","ids=C;");
    const auto env=H::resolve_parameters(*f.hir);
    for(const auto mode:{A::ParameterMode::Effective,A::ParameterMode::Defaults}) {
        const auto r=V::check_gradients(f,env,{},{"A"},{},mode);
        require(r.passed(),"finite difference must honor parameter mode");
        close(r.rows[0].automatic,mode==A::ParameterMode::Effective?0:3,"mode gradient");
    }
    require(V::check_gradients(f,env,{},{"A","B"},{},A::ParameterMode::Defaults).passed(),
        "other selected seeds stay independent during finite difference");
    const auto fixed=H::resolve_parameters(*f.hir,{{"B",5.0}});
    require(V::check_gradients(f,fixed,{},{"A"},{},A::ParameterMode::Defaults).passed(),"explicit override stays fixed");
}
void failures() {
    V::GradientOptions opts;
    const auto bounded=model("parameter real P=0 from [0:inf);","ids=P*2;");
    auto r=V::check_gradients(bounded,H::resolve_parameters(*bounded.hir),{},{"P"});
    require(r.has_errors()&&code(r.diagnostics,"GRADCHECK_RANGE"),"range crossing is not a successful check");
    const auto branch=model("parameter real P=.1;","begin if(P>0) ids=P; else ids=-P; end");
    opts.steps={{"P",1.0}};
    r=V::check_gradients(branch,H::resolve_parameters(*branch.hir),{},{"P"},opts);
    require(r.has_errors()&&code(r.diagnostics,"GRADCHECK_PATH"),"MIR branch path crossing rejected");
    const auto absolute=model("parameter real P=.1;","ids=abs(P);");
    r=V::check_gradients(absolute,H::resolve_parameters(*absolute.hir),{},{"P"},opts);
    require(!r.has_errors()&&!r.passed()&&!r.rows[0].passed,"completed mismatch is reported as FAIL");
    opts.steps={{"unknown",1.0}};
    require(V::check_gradients(absolute,H::resolve_parameters(*absolute.hir),{},{"P"},opts).has_errors(),"unknown step seed");
    opts.steps.clear();opts.relative_tolerance=-1;
    require(V::check_gradients(absolute,H::resolve_parameters(*absolute.hir),{},{"P"},opts).has_errors(),"invalid tolerance");
    const auto huge=model("parameter real P=1e300;","ids=P;");
    opts={};opts.steps={{"P",1e-20}};
    r=V::check_gradients(huge,H::resolve_parameters(*huge.hir),{},{"P"},opts);
    require(r.has_errors()&&code(r.diagnostics,"GRADCHECK_STEP"),"rounded-away perturbation is not evidence");
}
}
int main() {
    try {model_grid();parameter_modes();failures();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"Gradient verification matrix, parameter modes and failure paths passed\n";
}
