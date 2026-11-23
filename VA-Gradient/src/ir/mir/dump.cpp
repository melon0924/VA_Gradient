#include "vagrad/ir/mir/mir.hpp"
#include <ostream>

namespace vagrad::mir {
std::string_view opcode_name(Opcode op) {
    switch(op) {
    case Opcode::Constant:return "constant";case Opcode::LoadParam:return "load_param";
    case Opcode::LoadLocal:return "load_local";case Opcode::StoreLocal:return "store_local";
    case Opcode::Convert:return "convert";case Opcode::Unary:return "unary";case Opcode::Binary:return "binary";
    case Opcode::ReadPotential:return "read_potential";case Opcode::Temperature:return "read_temperature";
    case Opcode::MathCall:return "math";case Opcode::DiagnosticCall:return "diagnostic";
    case Opcode::ContributeFlow:return "contribute_flow";
    case Opcode::Undef:return "unassigned";
    case Opcode::Phi:return "phi";
    case Opcode::RequireDefined:return "require_defined";
    }
    return "<invalid>";
}
namespace {
void print_probe(std::ostream& out,const MirFunction& f,const hir::Probe& p) {
    auto name=[&](std::optional<hir::SymbolId> id){return id?f.hir->bindings->symbol(*id).name:std::string("0");};
    out<<" ("<<name(p.positive)<<", "<<name(p.negative)<<')';
    if(p.alias) out<<" alias="<<name(p.alias);
}
}
void print_mir(std::ostream& out,const MirFunction& f) {
    const auto ds=verify_mir(f);
    out<<(f.form==MirForm::Ssa?"MIR-SSA":"MIR-Local")<<" module="<<(f.hir?f.hir->name:"<none>")<<" blocks="<<f.blocks.size()
       <<" locals="<<f.locals.size()<<" values="<<f.values.size()<<" verified="<<(ds.empty()?"true":"false")<<'\n';
    if(!ds.empty()) {
        for(const auto& d:ds) out<<"Diagnostic "<<d.code<<": "<<d.message<<'\n';
        return;
    }
    print_source_map(out,f.hir->bindings->syntax.input());
    for(std::size_t k=0;k<f.locals.size();++k) {
        const auto& l=f.locals[k];
        out<<"local"<<k<<": "<<hir::type_name(l.type)<<" = "
           <<(l.symbol?f.hir->bindings->symbol(*l.symbol).name:"<merge temporary>")<<'\n';
    }
    out<<"entry bb"<<f.entry->value<<'\n';
    for(std::size_t b=0;b<f.blocks.size();++b) {
        out<<"bb"<<b<<":\n";
        for(const auto& i:f.blocks[b].instructions) {
            out<<"  ";
            if(i.result) out<<'v'<<i.result->value<<": "<<hir::type_name(f.values[i.result->value].type)<<" = ";
            out<<opcode_name(i.opcode);
            if(!i.op.empty()) out<<' '<<i.op;
            if(i.opcode==Opcode::Constant) out<<' '<<hir::scalar_text(i.literal);
            if(i.parameter) out<<" ["<<i.parameter->value<<"] "<<f.hir->bindings->symbol(*i.parameter).name;
            if(i.local) out<<" local"<<i.local->value;
            if(i.probe) print_probe(out,f,*i.probe);
            for(const auto v:i.operands) out<<" v"<<v.value;
            for(const auto& input:i.incoming) out<<" [bb"<<input.predecessor.value<<": v"<<input.value.value<<']';
            out<<" @node="<<i.origin.node.value<<" token="<<i.origin.token_index<<'\n';
        }
        const auto& t=*f.blocks[b].terminator;
        out<<"  ";
        switch(t.kind) {
        case TerminatorKind::Jump:out<<"jump bb"<<t.first->value;break;
        case TerminatorKind::Branch:out<<"branch v"<<t.condition->value<<" ? bb"<<t.first->value<<" : bb"<<t.second->value;break;
        case TerminatorKind::Return:out<<"return";break;
        case TerminatorKind::Unreachable:out<<"unreachable";break;
        }
        for(const auto& output:t.outputs)
            out<<" "<<f.hir->bindings->symbol(output.symbol).name<<"@exit=v"<<output.value.value;
        out<<" @node="<<t.origin.node.value<<" token="<<t.origin.token_index<<'\n';
    }
}
void print_cfg(std::ostream& out,const MirFunction& f,const CfgInfo& cfg) {
    out<<"CFG module="<<(f.hir?f.hir->name:"<none>")<<" blocks="<<f.blocks.size()<<'\n';
    if(cfg.has_errors()) {
        for(const auto& d:cfg.diagnostics) out<<"Diagnostic "<<d.code<<": "<<d.message<<'\n';
        return;
    }
    if(cfg.predecessors.size()!=f.blocks.size()||cfg.successors.size()!=f.blocks.size()||
       cfg.reachable.size()!=f.blocks.size()) {
        out<<"Diagnostic MIR_CFG: CFG dimensions do not match MIR; rebuild the analysis\n";
        return;
    }
    for(std::size_t b=0;b<f.blocks.size();++b) {
        out<<"bb"<<b<<" reachable="<<(cfg.reachable[b]?"true":"false")<<" predecessors=[";
        for(const auto p:cfg.predecessors[b]) out<<" bb"<<p.value;
        out<<" ] successors=[";
        for(const auto s:cfg.successors[b]) out<<" bb"<<s.value;
        out<<" ]\n";
    }
    out<<"reverse_postorder:";
    for(const auto b:cfg.reverse_postorder) out<<" bb"<<b.value;
    out<<'\n';
}
} // namespace vagrad::mir
