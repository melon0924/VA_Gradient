#include "vagrad/ad/eval.hpp"
#include "mir_test_support.hpp"
#include <iostream>

namespace {
using namespace mir_test;
namespace A=vagrad::ad;
constexpr auto defaults=A::ParameterMode::Defaults;
M::MirFunction model(std::string declarations,std::string expression) {
    return M::promote_to_ssa(text("module m; "+declarations+
        " real ids; analog ids="+expression+"; endmodule"));
}
double derivative(const A::Result& r,std::size_t lane=0) {
    if(r.has_errors()) {A::print_result(std::cerr,r);throw std::runtime_error("parameter AD failed");}
    return r.observations.at("ids")[lane];
}
void chains() {
    const auto f=model("parameter real A=2; parameter real B=A*3; localparam real C=B+1;","C");
    const auto env=H::resolve_parameters(*f.hir);
    close(derivative(A::evaluate(f,env,{},{"A"})),0,"effective values are independent");
    close(derivative(A::evaluate(f,env,{},{"A"},defaults)),3,"transitive defaults");
    close(derivative(A::evaluate(f,env,{},{"B"},defaults)),1,"seed interrupts its own default");
    const auto joint=A::evaluate(f,env,{},{"A","B"},defaults);
    close(derivative(joint),0,"joint independent seeds A");
    close(derivative(joint,1),1,"joint independent seeds B");
    const auto overridden=H::resolve_parameters(*f.hir,{{"B",5.0}});
    require(overridden.overridden.size()==1,"explicit override provenance");
    close(derivative(A::evaluate(f,overridden,{},{"A"},defaults)),0,"unselected override freezes chain");
    close(derivative(A::evaluate(f,overridden,{},{"B"},defaults)),1,"overridden selected parameter is a seed");

    const auto cyclic=model("parameter real A=B; parameter real B=A;","B");
    const auto broken=H::resolve_parameters(*cyclic.hir,{{"A",2.0}});
    require(!broken.has_errors(),"override breaks default cycle");
    close(derivative(A::evaluate(cyclic,broken,{},{"A"},defaults)),1,"derivative on broken cycle");
}
void numerical() {
    const auto f=model("parameter real A=2; parameter real B=ln(A*A+1); "
        "localparam real C=exp(B); parameter real D=A>0?C:sqrt(-A);","D/(1+A)");
    for(const double a:{-2.0,-1.5,.4,1.3,2.0}) {
        const auto env=H::resolve_parameters(*f.hir,{{"A",a}});
        const auto r=A::evaluate(f,env,{},{"A"},defaults);
        const double h=1e-5;
        const auto plus=M::evaluate_mir(f,H::resolve_parameters(*f.hir,{{"A",a+h}}),{});
        const auto minus=M::evaluate_mir(f,H::resolve_parameters(*f.hir,{{"A",a-h}}),{});
        require(!plus.has_errors()&&!minus.has_errors(),"default chain finite difference");
        const auto fd=(H::number(plus.observations.at("ids"))-H::number(minus.observations.at("ids")))/(2*h);
        require(std::abs(derivative(r)-fd)<1e-8,"default chain and finite difference");
        if(a==2) close(derivative(r),7.0/9,"closed form chain");
    }
    // 两个相等但非零的活动逻辑操作数不在布尔转换边界。
    const auto logic=model("parameter real A=2; parameter real B=(A&&A)?3*A:sqrt(-A);","B");
    close(derivative(A::evaluate(logic,H::resolve_parameters(*logic.hir),{},{"A"},defaults)),3,"default logical expression");
    const auto angle=model("parameter real A=2; parameter real B=atan2(A,2);","B");
    close(derivative(A::evaluate(angle,H::resolve_parameters(*angle.hir),{},{"A"},defaults)),.25,
        "two argument default math rule");
}
void boundaries() {
    const auto f=model("parameter real A=0; parameter real B=sqrt(A);","B");
    const auto env=H::resolve_parameters(*f.hir);
    close(derivative(A::evaluate(f,env,{},{"A"})),0,"effective B avoids default differentiation");
    const auto r=A::evaluate(f,env,{},{"A"},defaults);
    require(r.has_errors()&&code(r.diagnostics,"AD_PARAMETER")&&r.diagnostics[0].primary.has_value(),
        "singular default points to original expression");
    const auto integers=model("parameter real A=2; parameter integer N=A;","N");
    require(A::evaluate(integers,H::resolve_parameters(*integers.hir),{},{"A"},defaults).has_errors(),
        "active integer default rejected");
    const auto unused=model("parameter real A=2; parameter integer N=A;","A");
    close(derivative(A::evaluate(unused,H::resolve_parameters(*unused.hir),{},{"A"},defaults)),1,
        "unused default does not block loaded value derivatives");
    const auto discontinuity=model("parameter real A=0; parameter real B=A>0?A:1;","B");
    require(A::evaluate(discontinuity,H::resolve_parameters(*discontinuity.hir),{},{"A"},defaults).has_errors(),
        "default branch boundary rejected");
}
}
int main() {
    try {chains();numerical();boundaries();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"Effective and default-chain parameter derivatives passed\n";
}
