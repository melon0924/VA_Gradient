#include "vagrad/ir/mir/mir.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>

namespace vagrad::mir {
namespace {
namespace H = hir;
class Lowerer {
public:
    explicit Lowerer(std::shared_ptr<const H::HirModule> source) : f_(std::move(source)) {}
    MirFunction run() {
        if (!f_.hir || f_.hir->has_errors() || !f_.hir->body) {
            f_.diagnostics.push_back({Severity::Error,"MIR_HIR_ERRORS",
                "MIR lowering requires valid executable HIR",std::nullopt,{}});
            return std::move(f_);
        }
        for (const auto& v : f_.hir->variables) {
            locals_[v.symbol.value] = LocalId{f_.locals.size()};
            f_.locals.push_back({v.type,v.symbol,v.origin});
        }
        current_ = block();
        f_.entry = current_;
        statement(*f_.hir->body);
        terminate({TerminatorKind::Return, f_.hir->stmt(*f_.hir->body).origin, {}, {}, {}});
        return std::move(f_);
    }
private:
    MirFunction f_;
    std::map<std::size_t,LocalId> locals_;
    BlockId current_{0};
    BlockId block() {
        const BlockId id{f_.blocks.size()}; f_.blocks.emplace_back(); return id;
    }
    void terminate(Terminator t) {
        auto& b = f_.blocks.at(current_.value);
        if (b.terminator) throw std::logic_error("MIR block already terminated");
        // 相同真假目标不保留多重边。
        if (t.kind == TerminatorKind::Branch && t.first == t.second) {
            t.kind = TerminatorKind::Jump; t.condition.reset(); t.second.reset();
        }
        b.terminator = std::move(t);
    }
    ValueId emit(Instruction i, H::Type type) {
        if (f_.blocks.at(current_.value).terminator) throw std::logic_error("instruction after terminator");
        const ValueId id{f_.values.size()};
        f_.values.push_back({type,i.origin}); i.result=id;
        f_.blocks[current_.value].instructions.push_back(std::move(i)); return id;
    }
    void effect(Instruction i) {
        if (f_.blocks.at(current_.value).terminator) throw std::logic_error("effect after terminator");
        f_.blocks[current_.value].instructions.push_back(std::move(i));
    }
    Instruction instruction(Opcode op, H::Origin origin) const {
        Instruction i; i.opcode=op; i.origin=origin; return i;
    }
    ValueId constant(std::int32_t n, H::Origin origin) {
        auto i=instruction(Opcode::Constant,origin); i.literal=n; return emit(std::move(i),H::Type::Integer);
    }
    void store(LocalId local,ValueId value,H::Origin origin) {
        auto i=instruction(Opcode::StoreLocal,origin); i.local=local; i.operands={value}; effect(std::move(i));
    }
    ValueId load(LocalId local,H::Origin origin) {
        auto i=instruction(Opcode::LoadLocal,origin); i.local=local; return emit(std::move(i),f_.locals[local.value].type);
    }
    // 表达式合流暂用隐藏 Local 槽；后续 SSA 阶段再插入 phi。
    ValueId selection(ValueId condition,H::Origin origin,H::Type type,
                      const std::function<ValueId()>& yes,const std::function<ValueId()>& no) {
        const LocalId slot{f_.locals.size()}; f_.locals.push_back({type,{},origin});
        const auto t=block(), f=block(), merge=block();
        terminate({TerminatorKind::Branch,origin,condition,t,f});
        current_=t; const auto tv=yes(); store(slot,tv,origin);
        terminate({TerminatorKind::Jump,origin,{},merge,{}});
        current_=f; const auto fv=no(); store(slot,fv,origin);
        terminate({TerminatorKind::Jump,origin,{},merge,{}});
        current_=merge; return load(slot,origin);
    }
    ValueId truth(ValueId v,H::Origin origin) {
        auto i=instruction(Opcode::Unary,origin); i.op="!"; i.operands={v};
        const auto inverted=emit(std::move(i),H::Type::Integer);
        auto j=instruction(Opcode::Unary,origin); j.op="!"; j.operands={inverted};
        return emit(std::move(j),H::Type::Integer);
    }
    ValueId expression(H::ExprId id) {
        const auto& e=f_.hir->expr(id);
        if (e.kind==H::ExprKind::Select) {
            const auto c=expression(e.operands[0]);
            return selection(c,e.origin,e.type,[&]{return expression(e.operands[1]);},
                             [&]{return expression(e.operands[2]);});
        }
        if (e.kind==H::ExprKind::Binary && (e.op=="&&" || e.op=="||")) {
            const auto c=expression(e.operands[0]);
            if(e.op=="&&") return selection(c,e.origin,H::Type::Integer,
                [&]{return truth(expression(e.operands[1]),e.origin);},[&]{return constant(0,e.origin);});
            return selection(c,e.origin,H::Type::Integer,[&]{return constant(1,e.origin);},
                [&]{return truth(expression(e.operands[1]),e.origin);});
        }
        auto i=instruction(Opcode::Constant,e.origin);
        i.op=e.op; i.literal=e.literal; i.probe=e.probe;
        if(e.kind==H::ExprKind::Ref) {
            const auto kind=f_.hir->bindings->symbol(*e.symbol).kind;
            if(kind==H::SymbolKind::Parameter || kind==H::SymbolKind::LocalParameter) {
                i.opcode=Opcode::LoadParam; i.parameter=e.symbol;
            } else { i.opcode=Opcode::LoadLocal; i.local=locals_.at(e.symbol->value); }
        } else {
            switch(e.kind) {
            case H::ExprKind::Literal: i.opcode=Opcode::Constant; break;
            case H::ExprKind::Convert: i.opcode=Opcode::Convert; break;
            case H::ExprKind::Unary: i.opcode=Opcode::Unary; break;
            case H::ExprKind::Binary: i.opcode=Opcode::Binary; break;
            case H::ExprKind::ReadPotential: i.opcode=Opcode::ReadPotential; break;
            case H::ExprKind::Temperature: i.opcode=Opcode::Temperature; break;
            case H::ExprKind::MathCall: i.opcode=Opcode::MathCall; break;
            case H::ExprKind::DiagnosticCall: i.opcode=Opcode::DiagnosticCall; break;
            default: throw std::logic_error("unsupported HIR expression reached MIR");
            }
            for(const auto operand:e.operands) i.operands.push_back(expression(operand));
        }
        if(e.type==H::Type::Void) throw std::logic_error("Void expression used as a value");
        return emit(std::move(i),e.type);
    }
    void statement(H::StmtId id) {
        const auto& s=f_.hir->stmt(id);
        switch(s.kind) {
        case H::StmtKind::Empty: break;
        case H::StmtKind::Block: for(const auto child:s.children) statement(child); break;
        case H::StmtKind::Assign: {
            const auto value=expression(*s.expression); store(locals_.at(s.target->value),value,s.origin); break;
        }
        case H::StmtKind::Contribute: {
            auto i=instruction(Opcode::ContributeFlow,s.origin); i.probe=s.probe;
            i.operands={expression(*s.expression)}; effect(std::move(i)); break;
        }
        case H::StmtKind::Expression: {
            const auto& e=f_.hir->expr(*s.expression);
            if(e.kind==H::ExprKind::DiagnosticCall) {
                auto i=instruction(Opcode::DiagnosticCall,e.origin);i.op=e.op;
                for(const auto arg:e.operands) i.operands.push_back(expression(arg));
                effect(std::move(i));
            } else (void)expression(*s.expression);
            break;
        }
        case H::StmtKind::If: {
            const auto c=expression(*s.expression);
            const auto t=block(),f=block(),merge=block();
            terminate({TerminatorKind::Branch,s.origin,c,t,f});
            current_=t; statement(s.children[0]); terminate({TerminatorKind::Jump,s.origin,{},merge,{}});
            current_=f; if(s.children.size()>1) statement(s.children[1]);
            terminate({TerminatorKind::Jump,s.origin,{},merge,{}});
            current_=merge; break;
        }
        }
    }
};
}
MirFunction lower_mir(std::shared_ptr<const hir::HirModule> source) { return Lowerer(std::move(source)).run(); }
bool MirFunction::has_errors() const {
    return !hir || hir->has_errors() || std::any_of(diagnostics.begin(),diagnostics.end(),
        [](const auto& d){return d.severity==Severity::Error;});
}
} // namespace vagrad::mir
