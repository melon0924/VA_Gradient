#include "vagrad/ad/forward.hpp"
#include "vagrad/ad/plan.hpp"
#include "vagrad/ad/state.hpp"
#include "vagrad/ad/eval.hpp"
#include "mir_test_support.hpp"
#include <cmath>
#include <iostream>

int main() {
    using namespace vagrad::ad;
    try {
        auto close=[](double a,double b){if(std::abs(a-b)>1e-12) throw Error("derivative mismatch");};
        const auto x=seed(2,2,0),y=seed(3,2,1);
        const auto f=binary("+",binary("*",x,x),y);
        close(f.value,7);close(f.tangent[0],4);close(f.tangent[1],1);
        const auto q=binary("/",x,y);
        close(q.tangent[0],1.0/3);close(q.tangent[1],-2.0/9);
        close(math("sqrt",seed(4,1,0)).tangent[0],.25);
        close(math("ln",math("exp",seed(.4,1,0))).tangent[0],1);
        close(select_extremum(true,x,y).tangent[1],1);
        bool rejected=false;
        try {(void)select_extremum(true,seed(0,1,0),constant(0,1));} catch(const Error&){rejected=true;}
        if(!rejected) throw Error("ambiguous derivative must be diagnosed");
        const auto f0=mir_test::text("module m; parameter real G=1m; parameter integer ENABLE=1; real ids; analog ids=G; endmodule");
        const auto s=vagrad::mir::promote_to_ssa(f0);
        const auto plan=make_plan(s,{"G"});if(plan.has_errors()) throw Error("valid seed plan");
        const auto env=vagrad::hir::resolve_parameters(*s.hir);
        const auto jet=parameter_jet(plan,env,plan.parameters[0]);
        close(jet.value,.001);close(jet.tangent[0],1);
        if(!make_plan(s,{"ENABLE"}).has_errors()||!make_plan(s,{"G","G"}).has_errors())
            throw Error("integer and duplicate seeds must be rejected");
        TangentState state(2,2);
        state.set({0},std::vector<double>{1,0});state.set({1},std::vector<double>{0,1});
        vagrad::mir::Instruction p0,p1;p0.opcode=p1.opcode=vagrad::mir::Opcode::Phi;
        p0.result=vagrad::mir::ValueId{0};p1.result=vagrad::mir::ValueId{1};
        p0.incoming={{{1},{1}}};p1.incoming={{{1},{0}}};
        state.apply_phi({p0,p1},{1});
        close(state.get({0})->at(1),1);close(state.get({1})->at(0),1);
        const auto model=vagrad::mir::promote_to_ssa(mir_test::sample());
        const auto parameters=vagrad::hir::resolve_parameters(*model.hir);
        const auto grad=evaluate(model,parameters,mir_test::inputs(),{"G0","VTH0","TNOM"});
        if(grad.has_errors()) {print_result(std::cerr,grad);throw Error("model AD failed");}
        close(grad.observations.at("ids")[0],.02625);
        close(grad.observations.at("ids")[1],-5e-5);
        close(grad.observations.at("ids")[2],0);
        close(grad.branch_flows.at("ds")[2],8.75e-8);
        const auto off=evaluate(model,parameters,mir_test::inputs(.3),{"G0","VTH0"});
        if(off.has_errors()) throw Error("cutoff selected constant branch is differentiable");
        close(off.observations.at("ids")[0],0);close(off.observations.at("ids")[1],0);
        const auto saturated=evaluate(model,parameters,mir_test::inputs(1,.8),{"G0","VTH0"});
        if(saturated.has_errors()) throw Error("saturation AD failed");
        close(saturated.observations.at("ids")[0],.15125);
        close(saturated.observations.at("ids")[1],-.00055);
        for(std::size_t lane=0;lane<grad.seeds.size();++lane) {
            const auto name=grad.seeds[lane];
            const double base=name=="G0"?.001:name=="VTH0"?.45:300;
            const double h=name=="G0"?1e-7:name=="VTH0"?1e-5:.01;
            const auto plus=vagrad::hir::resolve_parameters(*model.hir,{{name,base+h}});
            const auto minus=vagrad::hir::resolve_parameters(*model.hir,{{name,base-h}});
            const auto p=vagrad::mir::evaluate_mir(model,plus,mir_test::inputs());
            const auto m=vagrad::mir::evaluate_mir(model,minus,mir_test::inputs());
            if(p.has_errors()||m.has_errors()) throw Error("finite difference reference failed");
            const auto numerical=(p.branch_flows.at("ds")-m.branch_flows.at("ds"))/(2*h);
            if(std::abs(numerical-grad.branch_flows.at("ds")[lane])>1e-10+1e-6*std::abs(numerical))
                throw Error("AD and central difference mismatch");
        }
        const auto singular=vagrad::mir::promote_to_ssa(mir_test::text(
            "module m; parameter real P=0; real y; analog y=sqrt(P*P); endmodule"));
        const auto singular_env=vagrad::hir::resolve_parameters(*singular.hir);
        if(!evaluate(singular,singular_env,{},{"P"}).has_errors())
            throw Error("zero numerical tangent must not hide a singular active dependency");
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"Forward AD primitive values and seed derivatives passed\n";
}
