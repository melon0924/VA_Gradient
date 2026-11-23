#include "vagrad/ad/eval.hpp"
#include "mir_test_support.hpp"
#include <iostream>

namespace {
using namespace mir_test;namespace A=vagrad::ad;
void repeated() {
    auto f=M::promote_to_ssa(sample());
    const auto env=H::resolve_parameters(*f.hir);
    const auto prepared=A::prepare(f,env,{"G0","VTH0","TNOM"});
    require(!prepared.has_errors(),"prepare model");
    std::vector<A::BatchCase> batch;
    for(double vg:{.2,.7,1.2}) for(double g:{.001,.002})
        batch.push_back({H::resolve_parameters(*f.hir,{{"G0",g}}),inputs(vg)});
    auto bad=inputs();bad.node_voltages.erase("g");
    batch.insert(batch.begin()+2,{env,bad});
    const auto results=A::evaluate_batch(prepared,batch);
    require(results.size()==batch.size(),"batch preserves row order");
    for(std::size_t k=0;k<batch.size();++k) {
        if(k==2) {require(results[k].has_errors(),"one failing row remains an error");continue;}
        const auto direct=A::evaluate(f,batch[k].parameters,batch[k].inputs,{"G0","VTH0","TNOM"});
        require(!results[k].has_errors()&&results[k].observations==direct.observations&&results[k].branch_flows==direct.branch_flows,
            "prepared and one-shot derivatives agree");
        require(results[k].primal.observations==direct.primal.observations,"primal state isolated between rows");
    }
    f.blocks.clear();f.values.clear();
    require(!A::evaluate(prepared,env,inputs()).has_errors(),"prepared snapshot owns its MIR");
    require(A::evaluate_batch(prepared,{}).empty(),"empty batch");
}
void default_signature() {
    const auto f=M::promote_to_ssa(text("module m; parameter real A=2; parameter real B=A*3; "
        "localparam real C=B+1; real ids; analog ids=C; endmodule"));
    const auto env=H::resolve_parameters(*f.hir);
    const auto prepared=A::prepare(f,env,{"A"},A::ParameterMode::Defaults);
    const auto changed=H::resolve_parameters(*f.hir,{{"A",4.0}});
    require(prepared.accepts(changed),"changing a selected seed does not change structural support");
    const auto r=A::evaluate(prepared,changed,{});
    require(!r.has_errors(),"prepared default chain");
    close(r.observations.at("ids")[0],3,"reused default derivative");
    close(H::number(r.primal.observations.at("ids")),13,"reused default primal");
    const auto fixed=H::resolve_parameters(*f.hir,{{"B",5.0}});
    const auto rejected=A::evaluate(prepared,fixed,{});
    require(rejected.has_errors()&&code(rejected.diagnostics,"AD_ENV"),"changed default override signature requires preparation");
    const auto frozen=A::prepare(f,fixed,{"A"},A::ParameterMode::Defaults);
    const auto newvalue=H::resolve_parameters(*f.hir,{{"B",7.0}});
    close(A::evaluate(frozen,newvalue,{}).observations.at("ids")[0],0,"same frozen signature reuses plan");
    const auto invalid=A::prepare(f,env,{"unknown"});
    require(invalid.has_errors()&&A::evaluate(invalid,env,{}).has_errors(),"failed preparation stays failed");
}
}
int main() {
    try {repeated();default_signature();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"Prepared snapshot, batch isolation and default override signatures passed\n";
}
