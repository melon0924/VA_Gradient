#pragma once
#include "mir_test_support.hpp"
namespace mir_test {
inline M::MirFunction loop_graph() {
    auto f=text("module m; parameter integer N=2; integer i,x,y; analog begin i=0; x=1; y=2; end endmodule");
    const auto o=f.blocks[0].terminator->origin;
    auto local=[&](std::string_view name) {
        for(std::size_t k=0;k<f.locals.size();++k)
            if(f.hir->bindings->symbol(*f.locals[k].symbol).name==name) return M::LocalId{k};
        throw std::runtime_error("local");
    };
    const auto i=local("i"),x=local("x"),y=local("y");
    f.values.clear();f.blocks.assign(4,{});f.entry=M::BlockId{0};
    auto value=[&](std::size_t b,M::Opcode op,std::vector<M::ValueId> args={},std::optional<M::LocalId> l={},
                   std::string operation="",std::int32_t literal=0) {
        M::Instruction ins;ins.opcode=op;ins.origin=o;ins.operands=std::move(args);ins.local=l;ins.op=operation;ins.literal=literal;
        ins.result=M::ValueId{f.values.size()};f.values.push_back({H::Type::Integer,o});
        if(op==M::Opcode::LoadParam) ins.parameter=f.hir->parameters[0].symbol;
        const auto id=*ins.result;f.blocks[b].instructions.push_back(std::move(ins));return id;
    };
    auto store=[&](std::size_t b,M::LocalId l,M::ValueId v) {
        M::Instruction ins;ins.opcode=M::Opcode::StoreLocal;ins.origin=o;ins.local=l;ins.operands={v};
        f.blocks[b].instructions.push_back(std::move(ins));
    };
    const auto zero=value(0,M::Opcode::Constant),one=value(0,M::Opcode::Constant,{},{},"",1),
        two=value(0,M::Opcode::Constant,{},{},"",2);
    store(0,i,zero);store(0,x,one);store(0,y,two);
    const auto iv=value(1,M::Opcode::LoadLocal,{},i),n=value(1,M::Opcode::LoadParam);
    const auto condition=value(1,M::Opcode::Binary,{iv,n},{},"<");
    const auto oldx=value(2,M::Opcode::LoadLocal,{},x),oldy=value(2,M::Opcode::LoadLocal,{},y);
    store(2,x,oldy);store(2,y,oldx);
    const auto next=value(2,M::Opcode::Binary,{iv,one},{},"+");store(2,i,next);
    f.blocks[0].terminator=M::Terminator{M::TerminatorKind::Jump,o,{},M::BlockId{1},{}};
    f.blocks[1].terminator=M::Terminator{M::TerminatorKind::Branch,o,condition,M::BlockId{2},M::BlockId{3}};
    f.blocks[2].terminator=M::Terminator{M::TerminatorKind::Jump,o,{},M::BlockId{1},{}};
    f.blocks[3].terminator=M::Terminator{M::TerminatorKind::Return,o,{},{},{}};
    require(M::verify_mir(f).empty(),"manual loop valid");return f;
}
}
