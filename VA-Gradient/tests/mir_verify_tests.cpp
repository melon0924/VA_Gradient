#include "mir_test_support.hpp"
#include <iostream>
#include <sstream>

namespace {
using namespace mir_test;
M::MirFunction fixture() {
    return text("module m; real ids; analog begin ids=1; if(1) ids=2; else ids=3; end endmodule");
}
void cfg() {
    const auto m=fixture();const auto cfg=M::build_cfg(m);
    require(!cfg.has_errors()&&cfg.successors[0]==std::vector<M::BlockId>{{1},{2}},"branch edges");
    require(cfg.predecessors[3]==std::vector<M::BlockId>{{1},{2}},"merge predecessors");
    require(cfg.reverse_postorder.size()==4,"RPO visits reachable blocks");
    require(M::verify_mir(m).empty(),"lowered graph verifies");
    std::ostringstream out;M::print_mir(out,m);M::print_cfg(out,m,cfg);
    require(out.str().find("verified=true")!=std::string::npos&&out.str().find("reverse_postorder")!=std::string::npos,"dumps");
    auto island=m;
    const auto origin=m.blocks[0].terminator->origin;
    island.blocks.push_back({{},M::Terminator{M::TerminatorKind::Unreachable,origin,{},{},{}}});
    require(!M::build_cfg(island).reachable.back()&&M::verify_mir(island).empty(),"explicit unreachable island retained");
}
void negatives() {
    auto m=fixture();m.entry=M::BlockId{100};
    require(code(M::verify_mir(m),"MIR_ENTRY"),"invalid entry");
    m=fixture();m.blocks[1].terminator.reset();
    require(code(M::verify_mir(m),"MIR_TERMINATOR"),"missing terminator");
    m=fixture();m.blocks[0].terminator->first=M::BlockId{100};
    require(code(M::verify_mir(m),"MIR_TARGET"),"invalid target");
    m=fixture();m.blocks[0].terminator->second=m.blocks[0].terminator->first;
    require(code(M::verify_mir(m),"MIR_TERMINATOR"),"duplicate branch targets must be normalized");
    m=fixture();m.blocks[0].instructions[0].result=M::ValueId{100};
    require(code(M::verify_mir(m),"MIR_VALUE"),"invalid result");
    m=fixture();m.blocks[0].instructions[1].result=m.blocks[0].instructions[0].result;
    require(code(M::verify_mir(m),"MIR_DEFINITION"),"duplicate definition");
    m=fixture();m.blocks[0].instructions.back().local=M::LocalId{100};
    require(code(M::verify_mir(m),"MIR_LOCAL"),"invalid local");
    m=fixture();m.blocks[0].instructions[1].operands[0]=M::ValueId{100};
    require(code(M::verify_mir(m),"MIR_VALUE"),"invalid operand");
    m=fixture();m.values[0].type=H::Type::String;
    require(code(M::verify_mir(m),"MIR_TYPE"),"constant/result mismatch");
    m=fixture();std::swap(m.blocks[0].instructions[0],m.blocks[0].instructions[1]);
    require(code(M::verify_mir(m),"MIR_DOMINANCE"),"use before definition in same block");
    m=fixture();auto store=m.blocks[1].instructions.back();
    m.blocks[3].instructions.push_back(store);
    require(code(M::verify_mir(m),"MIR_DOMINANCE"),"branch-local value unavailable after merge");
    m=fixture();m.blocks[0].terminator->condition=m.blocks[1].instructions[1].result;
    require(code(M::verify_mir(m),"MIR_DOMINANCE"),"branch condition dominance");
    m=fixture();m.blocks[0].instructions[0].origin.token_index=999999;
    require(code(M::verify_mir(m),"MIR_ORIGIN"),"bad origin produces safe diagnostic");
    m=fixture();m.blocks[0].terminator->condition=M::ValueId{100};
    require(code(M::verify_mir(m),"MIR_CONDITION"),"condition range");
    m=fixture();m.locals[0].type=H::Type::String;
    require(code(M::verify_mir(m),"MIR_LOCAL"),"source local type");
    m=fixture();m.blocks[2].instructions[0].result.reset();
    require(code(M::verify_mir(m),"MIR_RESULT"),"result required");
    m=fixture();m.hir.reset();
    require(code(M::verify_mir(m),"MIR_HIR_ERRORS"),"missing HIR owner");
}
void sample_and_cycles() {
    for(const auto& opts:std::vector<PreprocessOptions>{{},{{},{{"SOFT_THRESHOLD","1"},{"TRACE_MOS","1"}}}}) {
        const auto m=sample(opts);
        const auto ds=M::verify_mir(m);
        if(!ds.empty()) throw std::runtime_error(ds[0].code+": "+ds[0].message);
        require(m.hir->parameters.size()==7,"params retained");
    }
    auto loop=text("module m; analog ; endmodule");
    const auto o=loop.blocks[0].terminator->origin;
    loop.blocks[0].terminator=M::Terminator{M::TerminatorKind::Jump,o,{},M::BlockId{0},{}};
    require(M::verify_mir(loop).empty(),"well-formed self-loop graph");
    const auto cfg=M::build_cfg(loop);
    require(cfg.predecessors[0]==std::vector<M::BlockId>{{0}}&&cfg.reverse_postorder.size()==1,"cycle CFG terminates");
    H::EvalInputs i;i.step_limit=5;
    const auto result=M::evaluate_mir(loop,H::resolve_parameters(*loop.hir),i);
    require(code(result.diagnostics,"EVAL_LIMIT")&&result.observations.empty(),"loop execution budget");
}
}
int main() {
    try {cfg();negatives();sample_and_cycles();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"CFG and MIR structural/type/dominance verification passed\n";
}
