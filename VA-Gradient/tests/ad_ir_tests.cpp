#include "vagrad/ad/ir.hpp"
#include "vagrad/ad/eval.hpp"
#include "vagrad/validation/gradcheck.hpp"
#include "mir_test_support.hpp"
#include <iostream>
#include <sstream>

namespace {
using namespace mir_test;namespace A=vagrad::ad;
void expressions() {
    const auto f=M::promote_to_ssa(text("module m; parameter real P=2; parameter real Q=3; "
        "real a,b,c,d,e; analog begin a=P+Q; b=P-Q; c=P*Q; d=P/Q; e=-P; end endmodule"));
    const auto ir=A::lower_derivatives(f);
    require(!ir.has_errors()&&A::verify_derivatives(f,ir).empty(),"derivative IR verifies");
    std::size_t checked=0;
    for(const auto& r:ir.rules) if(r.kind==A::DerivativeKind::Expression&&r.source==M::Opcode::Binary) {
        const auto a=A::seed(2,2,0),b=A::seed(3,2,1);
        const auto reference=A::binary(r.operation,a,b);
        const auto compiled=A::evaluate_derivative(r,{a,b},reference.value,2);
        require(compiled.tangent==reference.tangent,"compiled derivative DAG matches direct chain primitive");
        ++checked;
        auto broken=ir;
        auto& node=broken.rules[r.result.value].nodes.back();
        node.inputs[0]=broken.rules[r.result.value].nodes.size()-1;
        require(code(A::verify_derivatives(f,broken),"AD_IR"),"self/forward fold dependency rejected");
    }
    require(checked==4,"all arithmetic local derivative DAGs covered");
    auto wrong=ir;wrong.rules.front().result={f.values.size()};
    require(code(A::verify_derivatives(f,wrong),"AD_IR"),"wrong derivative definition rejected");
    wrong=ir;wrong.rules.pop_back();
    require(code(A::verify_derivatives(f,wrong),"AD_IR"),"incomplete derivative arena rejected");
    std::ostringstream out;A::print_derivatives(out,f,ir);
    require(out.str().find("primal_result")!=std::string::npos&&out.str().find("yield q")!=std::string::npos,
        "division consumes authoritative primal ratio and exposes expression order");
}
void model() {
    const auto f=M::promote_to_ssa(sample());
    const auto ir=A::lower_derivatives(f);require(!ir.has_errors(),"model derivative IR");
    std::size_t phis=0,parameters=0,intrinsics=0;
    for(const auto& r:ir.rules) {
        phis+=r.kind==A::DerivativeKind::Phi;
        parameters+=r.kind==A::DerivativeKind::Parameter;
        intrinsics+=r.kind==A::DerivativeKind::Extremum;
    }
    require(phis==3&&parameters>0&&intrinsics==1,"model rule categories retained");
    const auto r=A::evaluate(f,H::resolve_parameters(*f.hir),inputs(),{"G0","VTH0"});
    require(!r.has_errors(),"model executes generated derivative rules");
    close(r.observations.at("ids")[0],.02625,"generated model gradient");
    require(A::lower_derivatives(sample()).has_errors(),"derivative lowering requires SSA");
}
void math_intrinsics() {
    for(const std::string expression:{"sqrt(P+2)","exp(P)","ln(P+2)","log(P+2)","sin(P)","cos(P)","tan(P)",
        "asin(P)","acos(P)","atan(P)","atan2(P,2)","sinh(P)","cosh(P)","tanh(P)","asinh(P)","acosh(P+2)",
        "atanh(P)","abs(P)","ceil(P)","floor(P)","min(P,1)","max(P,1)","P**2.3","$vt(P+300)"}) {
        const auto f=M::promote_to_ssa(text("module m; parameter real P=.4; real ids; analog ids="+expression+"; endmodule"));
        const auto check=vagrad::validation::check_gradients(f,H::resolve_parameters(*f.hir),{},{"P"});
        if(!check.passed()) throw std::runtime_error("compiled math derivative and finite difference: "+expression);
    }
}
}
int main() {
    try {expressions();model();math_intrinsics();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"Derivative IR arithmetic, fold order, CFG rules and model execution passed\n";
}
