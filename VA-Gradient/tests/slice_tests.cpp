#include "mir_test_support.hpp"
#include "mir_loop_fixture.hpp"
#include "vagrad/ir/mir/slice.hpp"
#include <iostream>
#include <set>
#include <sstream>

namespace {
using namespace mir_test;
M::MirFunction ssa(M::MirFunction m) {
    auto s=M::promote_to_ssa(m);require(!s.has_errors()&&M::verify_mir(s).empty(),"valid SSA fixture");return s;
}
std::set<std::string> names(const M::SliceResult& s,const M::SliceInputs& inputs) {
    std::set<std::string> found;
    for(const auto p:inputs.parameters) found.insert(s.function.hir->bindings->symbol(p).name);
    return found;
}
M::SliceResult sliced(const M::MirFunction& s,std::string target,M::SliceEffects effects=M::SliceEffects::TargetOnly) {
    auto r=M::slice_ssa(s,std::move(target),effects);
    if(r.has_errors()) throw std::runtime_error(r.diagnostics.empty()?"slice failed":r.diagnostics[0].message);
    require(M::verify_mir(r.function).empty(),"slice verifies");
    require(r.function.blocks.size()==s.blocks.size(),"CFG blocks retained");
    return r;
}
void manifest_and_model() {
    const auto full=ssa(sample());
    const auto ids=sliced(full,"ids@exit"),flow=sliced(full,"flow(ds)@exit"),preserve=sliced(full,"ids@exit",M::SliceEffects::Preserve);
    const std::set<std::string> expected{"G0","VTH0","LEAK","ENABLE"};
    require(names(ids,ids.target_inputs)==expected,"ids exact target parameters");
    auto flow_expected=expected;flow_expected.insert("TNOM");
    require(names(flow,flow.target_inputs)==flow_expected&&flow.target_inputs.temperature,"flow includes temperature scale");
    require(!ids.target_inputs.temperature&&names(preserve,preserve.execution_inputs).contains("TNOM"),"target vs execution inputs");
    require(ids.retained_instructions<flow.retained_instructions&&flow.retained_instructions<flow.original_instructions,"slice removes unrelated instructions");
    require(!ids.cfg_has_cycle&&ids.function.hir==full.hir,"source identity retained");
    const auto env=H::resolve_parameters(*full.hir);
    for(double vg:{0.,.3,.45,.46,.8,1.2}) for(double vd:{0.,.01,.1,.8}) for(double temp:{300.,600.}) {
        const auto i=inputs(vg,vd,temp);const auto original=M::evaluate_mir(full,env,i);
        const auto a=M::evaluate_mir(ids.function,env,i),b=M::evaluate_mir(flow.function,env,i),c=M::evaluate_mir(preserve.function,env,i);
        require(!original.has_errors()&&!a.has_errors()&&!b.has_errors()&&!c.has_errors(),"slice model executes");
        close(H::number(a.observations.at("ids")),H::number(original.observations.at("ids")),"ids grid");
        close(b.branch_flows.at("ds"),original.branch_flows.at("ds"),"flow grid");
        require(a.observations.size()==1&&a.branch_flows.empty()&&b.observations.empty(),"projected outputs");
        require(c.branch_flows==original.branch_flows,"preserve keeps contributions");
    }
    auto no_temperature=inputs();no_temperature.temperature_kelvin.reset();
    require(!M::evaluate_mir(ids.function,env,no_temperature).has_errors(),"isolated ids needs no temperature");
    require(M::evaluate_mir(preserve.function,env,no_temperature).has_errors(),"preserve retains missing-input error");
    for(const auto& [name,value]:std::map<std::string,double>{{"G0",.002},{"VTH0",.6},{"TNOM",600.0}})
        for(const auto* s:{&ids,&flow}) {
            const auto p=H::resolve_parameters(*full.hir,{{name,value}});
            const auto a=M::evaluate_mir(full,p,inputs()),b=M::evaluate_mir(s->function,p,inputs());
            require(!b.has_errors(),"override executes slice");
            if(s==&ids) close(H::number(b.observations.at("ids")),H::number(a.observations.at("ids")),"override ids");
            else close(b.branch_flows.at("ds"),a.branch_flows.at("ds"),"override flow");
        }
    std::ostringstream out;M::print_slice(out,ids);
    require(out.str().find("target_inputs")!=std::string::npos&&out.str().find("Effect")!=std::string::npos,"reviewable slice report");
}
void controls_effects_and_errors() {
    const auto conditional=ssa(text(R"(module m; parameter integer C=1; real ids;
        analog if(C) ids=2; else ids=3; endmodule)"));
    const auto cut=sliced(conditional,"ids@exit");
    require(names(cut,cut.target_inputs).contains("C"),"control parameter retained through phi");
    for(int c:{0,1}) {
        const auto env=H::resolve_parameters(*conditional.hir,{{"C",std::int32_t{c}}});
        const auto value=M::evaluate_mir(cut.function,env,{});
        close(H::number(value.observations.at("ids")),c?2:3,"phi selects path");
    }
    PreprocessOptions opts;opts.defines={{"TRACE_MOS","1"},{"SOFT_THRESHOLD","1"}};
    const auto trace=ssa(sample(opts));
    const auto a=sliced(trace,"ids@exit",M::SliceEffects::Preserve),b=sliced(trace,"ids@exit");
    const auto p=H::resolve_parameters(*trace.hir);
    require(M::evaluate_mir(a.function,p,inputs()).records.size()==1,"preserve diagnostics");
    require(M::evaluate_mir(b.function,p,inputs()).records.empty(),"explicit target-only excludes diagnostics");
    require(std::any_of(b.effect_decisions.begin(),b.effect_decisions.end(),[](const auto& e){
        return e.opcode==M::Opcode::DiagnosticCall&&!e.retained;}),"exclusion is reported");
    const auto trapping=ssa(text("module m; real ids,x; analog begin ids=1; x=sqrt(-1); end endmodule"));
    const auto strict=sliced(trapping,"ids@exit",M::SliceEffects::Preserve),isolated=sliced(trapping,"ids@exit");
    const auto e=H::resolve_parameters(*trapping.hir);
    require(code(M::evaluate_mir(strict.function,e,{}).diagnostics,"EVAL_DOMAIN"),"preserve unrelated runtime failures");
    require(!M::evaluate_mir(isolated.function,e,{}).has_errors(),"explicit isolation may remove unrelated failing computation");
    const auto partial=ssa(text("module m; parameter integer C=0; real ids; analog if(C) ids=1; endmodule"));
    const auto r=sliced(partial,"ids@exit");
    require(M::evaluate_mir(r.function,H::resolve_parameters(*partial.hir),{}).observations.empty(),"unassigned observation not zero");
}
void flow_orientation_and_cycles() {
    const auto full=ssa(text(R"(module m(a,b); inout a,b; electrical a,b; branch(a,b) ab;
        parameter real P=2; real ids; analog begin ids=0; I(ab)<+P; I(b,a)<+1; end endmodule)"));
    const auto cut=sliced(full,"flow(ab)@exit");
    const auto env=H::resolve_parameters(*full.hir);
    close(M::evaluate_mir(cut.function,env,{}).branch_flows.at("ab"),1,"all oriented contributions selected");
    const auto loop=ssa(loop_graph());const auto part=sliced(loop,"x@exit");
    require(part.cfg_has_cycle,"cyclic slice not flattened");
    for(int n:{0,1,3}) {
        const auto p=H::resolve_parameters(*loop.hir,{{"N",std::int32_t{n}}});
        const auto a=M::evaluate_mir(loop,p,{}),b=M::evaluate_mir(part.function,p,{});
        require(!b.has_errors(),"cyclic slice executes");
        close(H::number(a.observations.at("x")),H::number(b.observations.at("x")),"loop target");
    }
    require(M::slice_ssa(full,"unknown@exit").has_errors(),"unknown target");
    require(M::slice_ssa(sample(),"ids@exit").has_errors(),"SSA required");
    require(M::slice_ssa(part.function,"x@exit").has_errors(),"nested projection rejected");
    auto bad=cut.function;bad.flow_projection=std::vector<H::SymbolId>{full.hir->parameters[0].symbol};
    require(code(M::verify_mir(bad),"SLICE_PROJECTION"),"invalid flow projection");
}
}
int main() {
    try {manifest_and_model();controls_effects_and_errors();flow_orientation_and_cycles();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"Target/control slices, manifests, effects, cycles and numerical equivalence passed\n";
}
