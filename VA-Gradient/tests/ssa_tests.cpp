#include "mir_test_support.hpp"
#include "mir_loop_fixture.hpp"
#include "vagrad/analysis/dominance.hpp"
#include <iostream>
#include <sstream>

namespace {
using namespace mir_test;
M::MirFunction ssa(const M::MirFunction& m) {
    auto s=M::promote_to_ssa(m);
    const auto ds=M::verify_mir(s);
    if(!ds.empty()) throw std::runtime_error(ds.front().code+": "+ds.front().message);
    require(s.form==M::MirForm::Ssa,"SSA form");
    for(const auto& b:s.blocks) for(const auto& i:b.instructions)
        require(i.opcode!=M::Opcode::LoadLocal&&i.opcode!=M::Opcode::StoreLocal,"all local loads/stores promoted");
    return s;
}
std::size_t phis(const M::MirFunction& m) {
    std::size_t count=0;
    for(const auto& b:m.blocks) for(const auto& i:b.instructions) count+=i.opcode==M::Opcode::Phi;
    return count;
}
M::Instruction& first_phi(M::MirFunction& m) {
    for(auto& b:m.blocks) for(auto& i:b.instructions) if(i.opcode==M::Opcode::Phi) return i;
    throw std::runtime_error("phi expected");
}
void equivalent(const H::EvalResult& a,const H::EvalResult& b) {
    require(a.has_errors()==b.has_errors(),"execution status");
    if(a.has_errors()) {
        require(a.diagnostics.front().code==b.diagnostics.front().code,"error category");
        require(a.observations.empty()&&b.observations.empty(),"no successful partial outputs");return;
    }
    require(a.observations.size()==b.observations.size()&&a.branch_flows.size()==b.branch_flows.size()&&
        a.endpoint_flows.size()==b.endpoint_flows.size()&&a.records.size()==b.records.size(),"output keys/records");
    for(const auto& [key,v]:a.observations) {
        const auto& w=b.observations.at(key);require(H::scalar_type(v)==H::scalar_type(w),"value type");
        if(H::scalar_type(v)==H::Type::String) require(v==w,"string value");
        else close(H::number(v),H::number(w),key);
    }
    for(const auto& [key,v]:a.branch_flows) close(v,b.branch_flows.at(key),key);
    for(const auto& [key,v]:a.endpoint_flows) close(v,b.endpoint_flows.at(key),key);
    for(std::size_t k=0;k<a.records.size();++k) {
        require(a.records[k].function==b.records[k].function&&
            a.records[k].origin.token_index==b.records[k].origin.token_index&&
            a.records[k].arguments==b.records[k].arguments,"diagnostic order, arguments and origin");
    }
}
H::EvalResult align(const M::MirFunction& m,const M::MirFunction& s,const H::ParamEnv& p,const H::EvalInputs& i) {
    const auto a=H::evaluate_hir(*m.hir,p,i),b=M::evaluate_mir(m,p,i),c=M::evaluate_mir(s,p,i);
    equivalent(a,b);equivalent(b,c);return c;
}
M::MirFunction diamond() {
    return text("module m; parameter integer C=1; real ids; analog if(C) ids=2; else ids=3; endmodule");
}
void dominance_and_structure() {
    const auto m=diamond();const auto d=M::analyze_dominance(m);
    require(!d.has_errors()&&d.immediate_dominator[3]==M::BlockId{0},"merge immediate dominator");
    require(d.frontier[1]==std::vector<M::BlockId>{{3}}&&d.frontier[2]==std::vector<M::BlockId>{{3}},"diamond frontier");
    require(d.dominates({0},{3})&&!d.dominates({1},{3}),"dominance relation");
    const auto s=ssa(m);
    require(phis(s)==1&&m.form==M::MirForm::Local,"single phi and immutable input");
    const auto same=text(R"(module m; parameter integer C=1; real x,ids;
        analog begin x=1; if(C) ids=x; else ids=x; end endmodule)");
    require(phis(ssa(same))==0,"trivial identical-value phi removed");
    const auto dead=text(R"(module m; parameter integer C=1;
        analog begin:calc real t; if(C) t=1; else t=2; end endmodule)");
    require(phis(ssa(dead))==0,"liveness prunes unused local phi");
    auto unreachable=m;
    const auto o=m.blocks[0].terminator->origin;
    unreachable.blocks.push_back({{},M::Terminator{M::TerminatorKind::Unreachable,o,{},{},{}}});
    const auto u=ssa(unreachable);
    require(!M::build_cfg(u).reachable.back(),"unreachable island remains explicit");
    std::ostringstream out;M::print_mir(out,s);M::print_dominance(out,d);
    require(out.str().find("MIR-SSA")!=std::string::npos&&out.str().find("phi local")!=std::string::npos&&
        out.str().find("ids@exit=v")!=std::string::npos,"SSA and dominance reports");
}
void model_and_paths() {
    const auto m=sample(),s=ssa(m);
    require(phis(s)==3,"sample has condition phi and two ids phis");
    const auto defaults=H::resolve_parameters(*m.hir);
    struct Case{double vg,vd,t;int enable;double ids,flow;};
    for(const auto& c:std::vector<Case>{{.3,.05,300,1,5e-14,5e-14},{1,.05,300,1,2.625e-5,2.625e-5},
        {1,.8,300,1,1.5125e-4,1.5125e-4},{1,.55,300,1,1.5125e-4,1.5125e-4},
        {1,.05,300,0,5e-14,5e-14},{1,.05,600,1,2.625e-5,1.3125e-5}}) {
        const auto p=c.enable?defaults:H::resolve_parameters(*m.hir,{{"ENABLE",std::int32_t{0}}});
        const auto r=align(m,s,p,inputs(c.vg,c.vd,c.t));
        close(H::number(r.observations.at("ids")),c.ids,"SSA hand ids");close(r.branch_flows.at("ds"),c.flow,"SSA hand flow");
    }
    for(double vg:{0.,.3,.45,.46,.8,1.2}) for(double vd:{0.,.01,.1,.8}) (void)align(m,s,defaults,inputs(vg,vd,350));
    for(const auto& [name,v]:std::map<std::string,double>{{"G0",.002},{"VTH0",.6},{"TNOM",600},{"L",2e-6}})
        (void)align(m,s,H::resolve_parameters(*m.hir,{{name,v}}),inputs());
    PreprocessOptions opts;opts.defines={{"SOFT_THRESHOLD","1"},{"TRACE_MOS","1"}};
    const auto alt=sample(opts);(void)align(alt,ssa(alt),H::resolve_parameters(*alt.hir),inputs());
    auto missing=inputs();missing.node_voltages.erase("g");
    require(align(m,s,defaults,missing).has_errors(),"missing input");
    const auto nested=text(R"(module m; parameter integer C=1; parameter string S="ok";
        real ids,x; string label; analog begin ids=1; if(0&&sqrt(-1)) ids=9;
        if(1||(1.0/0.0)) ids+=2; x=C?4:sqrt(-1); ids+=x; label=S; end endmodule)");
    (void)align(nested,ssa(nested),H::resolve_parameters(*nested.hir),{});
    const auto partial=text("module m; parameter integer C=0; real ids; analog if(C) ids=1; endmodule");
    const auto ps=ssa(partial);
    const auto off=align(partial,ps,H::resolve_parameters(*partial.hir),{});
    require(off.observations.empty(),"unassigned observation remains absent");
    require(!align(partial,ps,H::resolve_parameters(*partial.hir,{{"C",std::int32_t{1}}}),{}).observations.empty(),"assigned observation present");
    const auto strings=text(R"(module m; parameter integer C=1; string label;
        analog label=C?"yes":"no"; endmodule)");
    (void)align(strings,ssa(strings),H::resolve_parameters(*strings.hir),{});
    for(const auto& expr:{"ids","ids+1","1.0/0.0","sqrt(-1.0)"}) {
        const auto bad=text("module m; real ids; analog "+std::string(expr)+"; endmodule");
        require(align(bad,ssa(bad),H::resolve_parameters(*bad.hir),{}).has_errors(),"discarded read/error remains observable");
    }
}
// 手工 MIR 回边测试：VA 的循环前端仍不在当前 HIR 支持范围。
void loops_and_phi_parallelism() {
    const auto m=loop_graph(),s=ssa(m);
    const auto d=M::analyze_dominance(m);
    require(std::find(d.frontier[2].begin(),d.frontier[2].end(),M::BlockId{1})!=d.frontier[2].end(),"loop frontier");
    require(phis(s)>=3,"loop-carried i,x,y phis");
    for(int count:{0,1,2,3,7}) {
        const auto p=H::resolve_parameters(*m.hir,{{"N",std::int32_t{count}}});
        const auto a=M::evaluate_mir(m,p,{}),b=M::evaluate_mir(s,p,{});
        equivalent(a,b);require(!b.has_errors(),"loop executes");
        close(H::number(b.observations.at("i")),count,"loop count");
        close(H::number(b.observations.at("x")),count%2?2:1,"parallel phi x");
        close(H::number(b.observations.at("y")),count%2?1:2,"parallel phi y");
    }
    auto entry_loop=m;entry_loop.entry=M::BlockId{1};entry_loop.blocks[0].instructions.clear();
    entry_loop.blocks[0].terminator->kind=M::TerminatorKind::Unreachable;entry_loop.blocks[0].terminator->first.reset();
    // 常量仍有定义，但取消变量初始化：第一次读取必须保持未赋值错误。
    std::vector<M::Instruction> constants;
    for(const auto& ins:m.blocks[0].instructions) if(ins.opcode==M::Opcode::Constant) constants.push_back(ins);
    entry_loop.blocks[1].instructions.insert(entry_loop.blocks[1].instructions.begin(),constants.begin(),constants.end());
    const auto entry_ssa=ssa(entry_loop);
    const auto env=H::resolve_parameters(*entry_loop.hir);
    const auto er=M::evaluate_mir(entry_loop,env,{}),esr=M::evaluate_mir(entry_ssa,env,{});
    equivalent(er,esr);require(code(esr.diagnostics,"EVAL_STATE"),"entry loop preserves unassigned state");
    auto empty=text("module m; analog ; endmodule");
    empty.blocks[0].terminator->kind=M::TerminatorKind::Jump;empty.blocks[0].terminator->first=M::BlockId{0};
    const auto es=ssa(empty);
    require(es.entry->value!=0&&M::build_cfg(es).predecessors[es.entry->value].empty(),"entry preheader normalization");
    H::EvalInputs limit;limit.step_limit=5;
    require(code(M::evaluate_mir(es,H::resolve_parameters(*es.hir),limit).diagnostics,"EVAL_LIMIT"),"SSA cycle budget");
}
void negatives() {
    const auto good=ssa(diamond());auto bad=good;
    first_phi(bad).incoming.pop_back();
    require(code(M::verify_mir(bad),"SSA_PHI_INPUT"),"missing phi edge");
    bad=good;first_phi(bad).incoming[1].predecessor=first_phi(bad).incoming[0].predecessor;
    require(code(M::verify_mir(bad),"SSA_PHI_INPUT"),"duplicate phi edge");
    bad=good;first_phi(bad).incoming[0].value=M::ValueId{999999};
    require(code(M::verify_mir(bad),"SSA_PHI_INPUT"),"invalid phi value");
    bad=good;first_phi(bad).incoming[0].value=first_phi(bad).incoming[1].value;
    require(code(M::verify_mir(bad),"MIR_DOMINANCE"),"phi edge availability");
    // 在 phi 前插入普通指令，破坏块首约束。
    bad=good;
    for(auto& b:bad.blocks) if(!b.instructions.empty()&&b.instructions[0].opcode==M::Opcode::Phi) {
        M::Instruction check;check.opcode=M::Opcode::RequireDefined;check.origin=b.instructions[0].origin;
        check.local=b.instructions[0].local;check.operands={*b.instructions[0].result};
        b.instructions.insert(b.instructions.begin(),check);break;
    }
    require(code(M::verify_mir(bad),"SSA_PHI_POSITION"),"phi placement");
    bad=good;for(auto& b:bad.blocks) if(b.terminator->kind==M::TerminatorKind::Return) b.terminator->outputs.clear();
    require(code(M::verify_mir(bad),"SSA_OUTPUT"),"explicit outputs required");
    bad=good;bad.form=M::MirForm::Local;
    require(code(M::verify_mir(bad),"SSA_FORM"),"form mismatch");
    const auto again=M::promote_to_ssa(good);
    require(again.has_errors()&&code(M::verify_mir(again),"SSA_FORM"),"promotion diagnostic remains visible");
}
}
int main() {
    try {dominance_and_structure();model_and_paths();loops_and_phi_parallelism();negatives();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"Dominance, pruned SSA, phi edges, parallel loops and three-way execution passed\n";
}
