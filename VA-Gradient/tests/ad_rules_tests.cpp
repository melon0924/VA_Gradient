#include "vagrad/ad/eval.hpp"
#include "mir_test_support.hpp"
#include <iostream>
#include <tuple>

namespace {
using namespace mir_test;
namespace A=vagrad::ad;
M::MirFunction expression(std::string expr) {
    return M::promote_to_ssa(text("module m; parameter real P=.4; parameter real Q=1.3; real ids; analog ids="+expr+"; endmodule"));
}
double primal(const M::MirFunction& f,const std::map<std::string,H::Scalar>& p) {
    const auto r=M::evaluate_mir(f,H::resolve_parameters(*f.hir,p),{});
    require(!r.has_errors(),"finite difference primal");return H::number(r.observations.at("ids"));
}
void finite_difference(const std::string& expression_text,double p,double q,const std::vector<std::string>& seeds={"P"}) {
    const auto f=expression(expression_text);
    const std::map<std::string,H::Scalar> values{{"P",p},{"Q",q}};
    const auto result=A::evaluate(f,H::resolve_parameters(*f.hir,values),{},seeds);
    if(result.has_errors()) {A::print_result(std::cerr,result);throw std::runtime_error(expression_text);}
    for(std::size_t k=0;k<seeds.size();++k) {
        const auto base=seeds[k]=="P"?p:q;const auto h=1e-5*std::max(1.0,std::abs(base));
        auto plus=values,minus=values;plus[seeds[k]]=base+h;minus[seeds[k]]=base-h;
        const auto numerical=(primal(f,plus)-primal(f,minus))/(2*h);
        const auto derivative=result.observations.at("ids")[k];
        require(std::abs(numerical-derivative)<=2e-8+1e-6*std::abs(numerical),"rule finite difference: "+expression_text);
    }
}
void smooth_rules() {
    for(const auto& name:{"sqrt","exp","ln","log","sin","cos","tan","asin","acos","atan",
        "sinh","cosh","tanh","asinh","atanh","abs","ceil","floor"})
        finite_difference(std::string(name)+"(P)",.4,1.3);
    finite_difference("acosh(P)",2.0,1.3);finite_difference("abs(P)",-.4,1.3);
    finite_difference("$vt(P)",300,1.3);
    finite_difference("atan2(P,Q)",.4,1.3,{"P","Q"});
    finite_difference("atan2(P,Q)",-.4,-1.3,{"P","Q"});
    finite_difference("P**Q",1.3,2.4,{"P","Q"});
    finite_difference("P**Q",-1.3,2,{"P"});
    finite_difference("max(P,Q)",2,1.3,{"P","Q"});
    finite_difference("min(P,Q)",.4,1.3,{"P","Q"});
    const auto tiny=A::binary("**",A::seed(1e-200,1,0),A::constant(2,1));
    require(tiny.tangent[0]>0&&std::abs(tiny.tangent[0]/2e-200-1)<1e-12,"power tangent survives primal underflow");
    const auto saturated=A::math("tanh",A::seed(20,1,0));
    require(saturated.tangent[0]>0&&std::abs(saturated.tangent[0]/(4*std::exp(-40.))-1)<1e-12,
        "tanh derivative survives rounded primal saturation");
}
void nonsmooth() {
    for(const auto& [expr,p,q,seeds]:std::vector<std::tuple<std::string,double,double,std::vector<std::string>>>{
        {"sqrt(P*P)",0,1,{"P"}},{"abs(P)",0,1,{"P"}},{"ceil(P)",1,1,{"P"}},
        {"floor(P)",1,1,{"P"}},{"atan2(P,Q)",0,0,{"P","Q"}},
        {"P**Q",-2,2,{"Q"}},{"max(P,Q)",1,1,{"P"}},{"P>Q",1,1,{"P"}}}) {
        const auto f=expression(expr);
        const auto result=A::evaluate(f,H::resolve_parameters(*f.hir,{{"P",p},{"Q",q}}),{},seeds);
        require(result.has_errors()&&code(result.diagnostics,"AD_RULE"),"boundary diagnostic: "+expr);
    }
    const auto direct=M::promote_to_ssa(text(
        "module m; parameter real P=0; real ids; analog if(P) ids=1; else ids=2; endmodule"));
    const auto r=A::evaluate(direct,H::resolve_parameters(*direct.hir),{},{"P"});
    require(r.has_errors()&&code(r.diagnostics,"AD_RULE"),"direct real branch boundary");
}
}
int main() {
    try {smooth_rules();nonsmooth();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"AD mathematical rules and singular dependency policies passed\n";
}
