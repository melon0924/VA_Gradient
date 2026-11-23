#include "vagrad/ad/eval.hpp"
#include "vagrad/ir/mir/slice.hpp"
#include "mir_test_support.hpp"
#include <iostream>

namespace {
using namespace mir_test;
namespace A=vagrad::ad;
void grid() {
    const auto f=M::promote_to_ssa(sample());
    const std::vector<std::string> seeds{"G0","VTH0","TNOM","LEAK","L"};
    const auto env=H::resolve_parameters(*f.hir);
    for(double vg:{.2,.4,.7,1.2}) for(double vd:{.01,.05,.6}) for(double t:{300.,600.}) {
        const auto input=inputs(vg,vd,t);
        const auto r=A::evaluate(f,env,input,seeds);
        if(r.has_errors()) {A::print_result(std::cerr,r);throw std::runtime_error("model grid AD");}
        const auto dense=A::evaluate(f,env,input,seeds,A::ParameterMode::Effective,A::Storage::Dense);
        require(!dense.has_errors()&&r.observations==dense.observations&&r.branch_flows==dense.branch_flows,
            "sparse and dense grid derivatives must match exactly");
        require(r.tangent_slots<r.dense_tangent_slots&&dense.tangent_slots==dense.dense_tangent_slots,
            "sparse layout must reduce persistent derivative slots");
        require(r.observations.at("ids")[4]==0,"unused parameter derivative");
        for(std::size_t k=0;k<4;++k) {
            const auto base=k==0?.001:k==1?.45:k==2?300:1e-12;
            const auto h=k==0?1e-7:k==1?1e-5:k==2?.01:1e-14;
            const auto p=H::resolve_parameters(*f.hir,{{seeds[k],base+h}});
            const auto m=H::resolve_parameters(*f.hir,{{seeds[k],base-h}});
            const auto plus=M::evaluate_mir(f,p,input),minus=M::evaluate_mir(f,m,input);
            require(!plus.has_errors()&&!minus.has_errors(),"grid difference primal");
            const auto numerical=(plus.branch_flows.at("ds")-minus.branch_flows.at("ds"))/(2*h);
            require(std::abs(numerical-r.branch_flows.at("ds")[k])<=1e-9+1e-5*std::abs(numerical),"grid gradient difference");
        }
    }
    const auto cut=M::slice_ssa(f,"ids@exit",M::SliceEffects::TargetOnly);
    require(!cut.has_errors(),"AD slice setup");
    auto input=inputs();input.temperature_kelvin.reset();
    const auto r=A::evaluate(cut.function,env,input,{"G0","VTH0","TNOM"});
    require(!r.has_errors()&&r.branch_flows.empty(),"target-only AD without temperature");
    close(r.observations.at("ids")[2],0,"excluded temperature derivative");
    PreprocessOptions options;options.defines={{"SOFT_THRESHOLD","1"},{"TRACE_MOS","1"}};
    const auto alt=M::promote_to_ssa(sample(options));
    const auto a=A::evaluate(alt,H::resolve_parameters(*alt.hir),inputs(),{"G0","VTH0"});
    require(!a.has_errors()&&a.primal.records.size()==1,"AD preserves configured diagnostic records");
}
M::MirFunction real_loop() {
    auto f=text("module m; parameter integer N=3; parameter real P=2; integer i; real x,y; analog begin i=0; x=P; y=1; end endmodule");
    const auto o=f.blocks[0].terminator->origin;
    auto local=[&](std::string_view name) {
        for(std::size_t k=0;k<f.locals.size();++k) if(f.hir->bindings->symbol(*f.locals[k].symbol).name==name) return M::LocalId{k};
        throw std::runtime_error("local missing");
    };
    auto parameter=[&](std::string_view name) {
        for(const auto& p:f.hir->parameters) if(f.hir->bindings->symbol(p.symbol).name==name) return p.symbol;
        throw std::runtime_error("parameter missing");
    };
    const auto i=local("i"),x=local("x"),y=local("y");
    f.values.clear();f.blocks.assign(4,{});f.entry=M::BlockId{0};
    auto emit=[&](std::size_t b,M::Opcode op,H::Type type,std::vector<M::ValueId> args={},
                  std::optional<M::LocalId> slot={},std::optional<H::SymbolId> param={},H::Scalar literal=std::int32_t{0},std::string operation="") {
        M::Instruction ins;ins.opcode=op;ins.origin=o;ins.operands=std::move(args);ins.local=slot;ins.parameter=param;
        ins.literal=std::move(literal);ins.op=operation;ins.result=M::ValueId{f.values.size()};
        f.values.push_back({type,o});const auto v=*ins.result;f.blocks[b].instructions.push_back(std::move(ins));return v;
    };
    auto store=[&](std::size_t b,M::LocalId slot,M::ValueId v) {
        M::Instruction ins;ins.opcode=M::Opcode::StoreLocal;ins.origin=o;ins.local=slot;ins.operands={v};f.blocks[b].instructions.push_back(ins);
    };
    const auto zero=emit(0,M::Opcode::Constant,H::Type::Integer),one=emit(0,M::Opcode::Constant,H::Type::Integer,{},{},{},std::int32_t{1});
    const auto realone=emit(0,M::Opcode::Constant,H::Type::Real,{},{},{},1.0);
    const auto p=emit(0,M::Opcode::LoadParam,H::Type::Real,{},{},parameter("P"));
    store(0,i,zero);store(0,x,p);store(0,y,realone);
    const auto iv=emit(1,M::Opcode::LoadLocal,H::Type::Integer,{},i),n=emit(1,M::Opcode::LoadParam,H::Type::Integer,{},{},parameter("N"));
    const auto c=emit(1,M::Opcode::Binary,H::Type::Integer,{iv,n},{},{},std::int32_t{0},"<");
    const auto oldx=emit(2,M::Opcode::LoadLocal,H::Type::Real,{},x),oldy=emit(2,M::Opcode::LoadLocal,H::Type::Real,{},y);
    const auto product=emit(2,M::Opcode::Binary,H::Type::Real,{oldx,p},{},{},0.0,"*");
    store(2,x,oldy);store(2,y,product);
    const auto next=emit(2,M::Opcode::Binary,H::Type::Integer,{iv,one},{},{},std::int32_t{0},"+");store(2,i,next);
    f.blocks[0].terminator=M::Terminator{M::TerminatorKind::Jump,o,{},M::BlockId{1},{}};
    f.blocks[1].terminator=M::Terminator{M::TerminatorKind::Branch,o,c,M::BlockId{2},M::BlockId{3}};
    f.blocks[2].terminator=M::Terminator{M::TerminatorKind::Jump,o,{},M::BlockId{1},{}};
    f.blocks[3].terminator=M::Terminator{M::TerminatorKind::Return,o,{},{},{}};
    require(M::verify_mir(f).empty(),"loop MIR valid");
    return M::promote_to_ssa(f);
}
void loops() {
    const auto f=real_loop();require(M::verify_mir(f).empty(),"loop SSA valid");
    for(int n:{0,1,2,3,4,7}) {
        const auto env=H::resolve_parameters(*f.hir,{{"N",std::int32_t{n}}});
        const auto r=A::evaluate(f,env,{},{"P"});require(!r.has_errors(),"loop AD executes");
        const auto dense=A::evaluate(f,env,{},{"P"},A::ParameterMode::Effective,A::Storage::Dense);
        require(!dense.has_errors()&&r.observations==dense.observations,"sparse loop fixed point agrees with dense");
        const auto ex=n%2==0?n/2+1:n/2,ey=n%2==0?n/2:n/2+2;
        close(r.observations.at("x")[0],ex==0?0:ex*std::pow(2.,ex-1),"loop x derivative");
        close(r.observations.at("y")[0],ey==0?0:ey*std::pow(2.,ey-1),"loop y derivative");
        const auto cut=M::slice_ssa(f,"x@exit",M::SliceEffects::TargetOnly);
        const auto sliced=A::evaluate(cut.function,env,{},{"P"});
        require(!sliced.has_errors(),"loop slice AD executes");
        close(sliced.observations.at("x")[0],r.observations.at("x")[0],"loop slice gradient equivalence");
    }
}
void sparse_layout() {
    A::TangentState state(std::vector<A::LaneMask>{1,2,0},2);
    state.set({0},std::vector<double>{5,0});state.set({1},std::vector<double>{0,7});
    state.set({2},std::vector<double>{0,0});
    require(state.get({2}).has_value(),"empty structural support is defined zero, not Undef");
    bool rejected=false;
    try {state.set({0},std::vector<double>{5,1});} catch(const A::Error&){rejected=true;}
    require(rejected,"under-approximated support must not silently discard a derivative");

    std::string source="module m;";
    std::vector<std::string> names;
    for(int k=0;k<64;++k) {names.push_back("P"+std::to_string(k));source+="parameter real "+names.back()+"=2;";}
    source+="real ids; analog ids=P0*P63; endmodule";
    const auto f=M::promote_to_ssa(text(source));
    const auto r=A::evaluate(f,H::resolve_parameters(*f.hir),{},names);
    require(!r.has_errors(),"64 seed mask boundary");
    const auto& d=r.observations.at("ids");
    for(std::size_t k=0;k<64;++k) close(d[k],k==0||k==63?2:0,"64 seed sparse gradient");
    require(r.tangent_slots<r.dense_tangent_slots/8,"many unused seeds remain sparse");
}
}
int main() {
    try {grid();loops();sparse_layout();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"AD bias grid, sliced output and loop-carried phi gradients passed\n";
}
