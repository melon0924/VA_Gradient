#include "vagrad/ir/mir/mir.hpp"
#include "vagrad/analysis/dominance.hpp"

#include <algorithm>
#include <set>

namespace vagrad::mir {
namespace {
namespace H = hir;
bool errors(const std::vector<Diagnostic>& ds) {
    return std::any_of(ds.begin(),ds.end(),[](const auto& d){return d.severity==Severity::Error;});
}
Diagnostic diagnostic(const MirFunction& f,std::optional<H::Origin> origin,std::string code,std::string message) {
    if(f.hir && f.hir->bindings && origin && origin->node.value<f.hir->bindings->syntax.node_count() &&
       origin->token_index<f.hir->bindings->syntax.input().tokens.size())
        return H::semantic_diagnostic(*f.hir->bindings,*origin,std::move(code),std::move(message));
    return {Severity::Error,std::move(code),std::move(message),std::nullopt,{}};
}
}
bool CfgInfo::has_errors() const {return errors(diagnostics);}
CfgInfo build_cfg(const MirFunction& f) {
    CfgInfo cfg;
    const auto n=f.blocks.size();
    cfg.predecessors.resize(n);cfg.successors.resize(n);cfg.reachable.resize(n);
    auto report=[&](std::optional<H::Origin> o,std::string code,std::string msg) {
        cfg.diagnostics.push_back(diagnostic(f,o,std::move(code),std::move(msg)));
    };
    if(!f.entry || f.entry->value>=n) report({},"MIR_ENTRY","missing or invalid entry block");
    for(std::size_t b=0;b<n;++b) {
        if(!f.blocks[b].terminator) {report({},"MIR_TERMINATOR","bb"+std::to_string(b)+" has no terminator");continue;}
        const auto& t=*f.blocks[b].terminator;
        bool shape=false;
        switch(t.kind) {
        case TerminatorKind::Jump:shape=t.first && !t.second && !t.condition;break;
        case TerminatorKind::Branch:shape=t.first && t.second && t.condition && t.first!=t.second;break;
        case TerminatorKind::Return:case TerminatorKind::Unreachable:shape=!t.first && !t.second && !t.condition;break;
        }
        if(!shape) {report(t.origin,"MIR_TERMINATOR","invalid terminator operands/targets");continue;}
        for(const auto target:{t.first,t.second}) if(target) {
            if(target->value>=n) {report(t.origin,"MIR_TARGET","branch target is outside block arena");continue;}
            cfg.successors[b].push_back(*target);
            cfg.predecessors[target->value].push_back({b});
        }
    }
    if(f.entry && f.entry->value<n) {
        // 显式 DFS 栈：长直线图不消耗 C++ 调用栈。
        std::vector<std::pair<BlockId,std::size_t>> stack{{*f.entry,0}};
        std::vector<BlockId> postorder;
        cfg.reachable[f.entry->value]=true;
        while(!stack.empty()) {
            auto& [b,next]=stack.back();
            if(next==cfg.successors[b.value].size()) {postorder.push_back(b);stack.pop_back();continue;}
            const auto child=cfg.successors[b.value][next++];
            if(!cfg.reachable[child.value]) {cfg.reachable[child.value]=true;stack.push_back({child,0});}
        }
        cfg.reverse_postorder.assign(postorder.rbegin(),postorder.rend());
    }
    return cfg;
}

std::vector<Diagnostic> verify_mir(const MirFunction& f) {
    auto cfg=build_cfg(f);
    auto ds=std::move(cfg.diagnostics);
    auto report=[&](std::optional<H::Origin> o,std::string code,std::string msg) {
        ds.push_back(diagnostic(f,o,std::move(code),std::move(msg)));
    };
    ds.insert(ds.end(),f.diagnostics.begin(),f.diagnostics.end());
    if(!f.hir || f.hir->has_errors()) report({},"MIR_HIR_ERRORS","MIR requires a valid HIR owner and successful lowering");
    if(!ds.empty()) return ds;
    const auto& h=*f.hir;
    if(f.form!=MirForm::Local&&f.form!=MirForm::Ssa) report({},"SSA_FORM","invalid MIR form");
    for(const auto* projection:{&f.output_projection,&f.flow_projection}) if(*projection) {
        if(f.form!=MirForm::Ssa) report({},"SLICE_PROJECTION","output projections require SSA");
        std::set<std::size_t> seen;
        for(const auto symbol:**projection) {
            const auto kind=projection==&f.output_projection?H::SymbolKind::Variable:H::SymbolKind::Branch;
            if(symbol.value>=h.bindings->symbols.size()||!seen.insert(symbol.value).second||
               h.bindings->symbol(symbol).kind!=kind||
               (kind==H::SymbolKind::Variable&&h.bindings->symbol(symbol).scope!=h.scope))
                report({},"SLICE_PROJECTION","invalid projected output symbol");
        }
    }
    if(f.form==MirForm::Ssa&&!cfg.predecessors[f.entry->value].empty())
        report({},"SSA_ENTRY","SSA entry requires a preheader without incoming edges");
    const auto valid_type=[](H::Type t){return H::numeric(t)||t==H::Type::String;};
    auto origin_ok=[&](H::Origin o) {
        return o.node.value<h.bindings->syntax.node_count() && o.token_index<h.bindings->syntax.input().tokens.size();
    };
    const auto n=f.blocks.size();
    if(n>2048 || f.values.size()>1000000) {report({},"MIR_LIMIT","verification limit exceeded (2048 blocks / 1000000 values)");return ds;}
    std::set<std::size_t> local_symbols;
    for(const auto& l:f.locals) {
        if(!valid_type(l.type) || !origin_ok(l.origin)) report(l.origin,"MIR_LOCAL","invalid local type/origin");
        if(l.symbol) {
            if(l.symbol->value>=h.bindings->symbols.size() ||
               h.bindings->symbol(*l.symbol).kind!=H::SymbolKind::Variable ||
               h.symbol_types[l.symbol->value]!=l.type || !local_symbols.insert(l.symbol->value).second)
                report(l.origin,"MIR_LOCAL","invalid, duplicated or mistyped source local");
        }
    }
    for(const auto& v:h.variables) if(!local_symbols.contains(v.symbol.value))
        report(v.origin,"MIR_LOCAL","source variable has no local slot");
    for(const auto& v:f.values)
        if(!valid_type(v.type)||!origin_ok(v.origin)) report(v.origin,"MIR_VALUE","invalid value type/origin");
    struct Def {std::size_t block,index;};
    std::vector<std::optional<Def>> definitions(f.values.size());
    for(std::size_t b=0;b<n;++b) {
        const auto& block=f.blocks[b];
        if(!origin_ok(block.terminator->origin)) report(block.terminator->origin,"MIR_ORIGIN","invalid terminator origin");
        if(!cfg.reachable[b] && block.terminator->kind!=TerminatorKind::Unreachable)
            report(block.terminator->origin,"MIR_UNREACHABLE","unreachable blocks must be explicitly marked Unreachable");
        bool ordinary_seen=false;
        for(std::size_t k=0;k<block.instructions.size();++k) {
            const auto& i=block.instructions[k];
            if(!origin_ok(i.origin)) report(i.origin,"MIR_ORIGIN","invalid instruction origin");
            const bool effect=i.opcode==Opcode::StoreLocal||i.opcode==Opcode::ContributeFlow||
                i.opcode==Opcode::DiagnosticCall||i.opcode==Opcode::RequireDefined;
            if(effect==i.result.has_value()) report(i.origin,"MIR_RESULT","effect/result instruction mismatch");
            const bool ssa_op=i.opcode==Opcode::Phi||i.opcode==Opcode::Undef||i.opcode==Opcode::RequireDefined;
            if((f.form==MirForm::Local&&ssa_op) || (f.form==MirForm::Ssa&&
                (i.opcode==Opcode::LoadLocal||i.opcode==Opcode::StoreLocal)))
                report(i.origin,"SSA_FORM","instruction does not belong to this MIR form");
            if(i.opcode==Opcode::Phi) {
                if(ordinary_seen) report(i.origin,"SSA_PHI_POSITION","phi must precede ordinary instructions");
            } else {
                ordinary_seen=true;
                if(!i.incoming.empty()) report(i.origin,"SSA_PHI_INPUT","only phi may have incoming edges");
            }
            if(i.result) {
                if(i.result->value>=f.values.size()) report(i.origin,"MIR_VALUE","invalid result ValueId");
                else if(definitions[i.result->value]) report(i.origin,"MIR_DEFINITION","ValueId has multiple definitions");
                else definitions[i.result->value]=Def{b,k};
            }
            for(const auto op:i.operands) if(op.value>=f.values.size()) report(i.origin,"MIR_VALUE","invalid operand ValueId");
            if(i.local && i.local->value>=f.locals.size()) report(i.origin,"MIR_LOCAL","invalid LocalId");
            if(i.parameter && i.parameter->value>=h.bindings->symbols.size()) report(i.origin,"MIR_PARAMETER","invalid parameter SymbolId");
            for(const auto& input:i.incoming)
                if(input.predecessor.value>=n||input.value.value>=f.values.size())
                    report(i.origin,"SSA_PHI_INPUT","invalid phi predecessor or ValueId");
        }
        const auto& t=*block.terminator;
        std::set<std::size_t> outputs;
        if((f.form==MirForm::Local||t.kind!=TerminatorKind::Return)&&!t.outputs.empty())
            report(t.origin,"SSA_OUTPUT","only SSA Return may carry outputs");
        for(const auto& output:t.outputs) {
            if(output.symbol.value>=h.bindings->symbols.size()||output.value.value>=f.values.size())
                report(t.origin,"SSA_OUTPUT","invalid output symbol or ValueId");
            else {
                const auto& symbol=h.bindings->symbol(output.symbol);
                if(symbol.kind!=H::SymbolKind::Variable||symbol.scope!=h.scope||
                    h.symbol_types[output.symbol.value]!=f.values[output.value.value].type||
                    !outputs.insert(output.symbol.value).second||
                    (f.output_projection&&std::find(f.output_projection->begin(),f.output_projection->end(),output.symbol)==f.output_projection->end()))
                    report(t.origin,"SSA_OUTPUT","duplicate, mistyped or non-module variable output");
            }
        }
        if(f.form==MirForm::Ssa&&t.kind==TerminatorKind::Return)
            for(const auto& variable:h.variables)
                if(h.bindings->symbol(variable.symbol).scope==h.scope&&!outputs.contains(variable.symbol.value)&&
                   (!f.output_projection||std::find(f.output_projection->begin(),f.output_projection->end(),variable.symbol)!=f.output_projection->end()))
                    report(t.origin,"SSA_OUTPUT","missing module variable output");
    }
    for(std::size_t v=0;v<definitions.size();++v) if(!definitions[v]) report(f.values[v].origin,"MIR_DEFINITION","ValueId has no definition");
    if(!ds.empty()) return ds; // 后续类型检查仅访问已经校验过的编号。
    const std::set<std::string> arithmetic{"+","-","*","/","**"};
    const std::set<std::string> compare{"<","<=",">",">=","==","!="};
    const std::set<std::string> integer_ops{"%","&","|","^","~^","^~","<<",">>","<<<",">>>"};
    auto probe_ok=[&](const H::Probe& p) {
        for(const auto net:{p.positive,p.negative}) if(net &&
            (net->value>=h.bindings->symbols.size() || h.bindings->symbol(*net).kind!=H::SymbolKind::Net)) return false;
        if(p.alias) {
            const auto it=std::find_if(h.branches.begin(),h.branches.end(),[&](const auto& b){return b.symbol==*p.alias;});
            if(it==h.branches.end()||it->probe.positive!=p.positive||it->probe.negative!=p.negative) return false;
        }
        return true;
    };
    for(std::size_t block_index=0;block_index<n;++block_index) {
        const auto& b=f.blocks[block_index];
        for(const auto& i:b.instructions) {
            const auto count=i.operands.size();
            const auto result=i.result ? f.values[i.result->value].type:H::Type::Void;
            auto arg=[&](std::size_t k){return f.values[i.operands[k].value].type;};
            bool ok=true;
            const bool local_op=i.opcode==Opcode::LoadLocal||i.opcode==Opcode::StoreLocal||
                i.opcode==Opcode::Phi||i.opcode==Opcode::Undef||i.opcode==Opcode::RequireDefined;
            if(local_op!=i.local.has_value() || (i.opcode==Opcode::LoadParam)!=i.parameter.has_value() ||
               ((i.opcode==Opcode::ReadPotential||i.opcode==Opcode::ContributeFlow)!=i.probe.has_value())) ok=false;
            if(i.probe && !probe_ok(*i.probe)) ok=false;
            switch(i.opcode) {
            case Opcode::Undef:
                ok=ok&&count==0&&i.local&&result==f.locals[i.local->value].type&&block_index==f.entry->value;break;
            case Opcode::RequireDefined:
                ok=ok&&count==1&&i.local&&arg(0)==f.locals[i.local->value].type;break;
            case Opcode::Phi: {
                ok=ok&&count==0&&i.local&&result==f.locals[i.local->value].type;
                std::set<std::size_t> predecessors;
                for(const auto& input:i.incoming) {
                    if(!predecessors.insert(input.predecessor.value).second||
                       f.values[input.value.value].type!=result) ok=false;
                }
                if(i.incoming.empty()||predecessors.size()!=cfg.predecessors[block_index].size()) ok=false;
                for(const auto pred:cfg.predecessors[block_index])
                    if(!predecessors.contains(pred.value)) ok=false;
                if(!ok) report(i.origin,"SSA_PHI_INPUT","phi must have one typed input per CFG predecessor");
                break;
            }
            case Opcode::Constant:ok=ok&&count==0&&H::scalar_type(i.literal)==result;break;
            case Opcode::LoadLocal:ok=ok&&count==0&&i.local&&result==f.locals[i.local->value].type;break;
            case Opcode::StoreLocal:ok=ok&&count==1&&i.local&&arg(0)==f.locals[i.local->value].type;break;
            case Opcode::LoadParam:
                ok=ok&&count==0&&i.parameter;
                if(ok) ok=std::any_of(h.parameters.begin(),h.parameters.end(),[&](const auto& p){return p.symbol==*i.parameter&&p.type==result;});
                break;
            case Opcode::Convert:ok=ok&&count==1&&H::numeric(arg(0))&&H::numeric(result);break;
            case Opcode::Unary:
                ok=ok&&count==1;
                if(ok) ok=(i.op=="!"&&H::numeric(arg(0))&&result==H::Type::Integer)||
                    ((i.op=="+"||i.op=="-")&&H::numeric(arg(0))&&arg(0)==result)||
                    (i.op=="~"&&arg(0)==H::Type::Integer&&result==H::Type::Integer);
                break;
            case Opcode::Binary:
                ok=ok&&count==2;
                if(ok) ok=(arithmetic.contains(i.op)&&H::numeric(arg(0))&&arg(0)==arg(1)&&result==arg(0))||
                    (compare.contains(i.op)&&H::numeric(arg(0))&&arg(0)==arg(1)&&result==H::Type::Integer)||
                    (integer_ops.contains(i.op)&&arg(0)==H::Type::Integer&&arg(1)==H::Type::Integer&&result==H::Type::Integer);
                break;
            case Opcode::ReadPotential:case Opcode::Temperature:ok=ok&&count==0&&result==H::Type::Real;break;
            case Opcode::ContributeFlow:ok=ok&&count==1&&arg(0)==H::Type::Real;break;
            case Opcode::MathCall:case Opcode::DiagnosticCall: {
                const auto& registry=H::builtin_registry();
                const auto s=std::find_if(registry.begin(),registry.end(),[&](const auto& sig){return sig.name==i.op;});
                ok=ok&&s!=registry.end();
                if(ok) {
                    ok=count>=s->min_args&&count<=s->max_args&&result==s->result&&
                        (i.opcode==Opcode::DiagnosticCall ? s->effect==H::Effect::Diagnostic :
                         s->effect==H::Effect::PureMath||s->effect==H::Effect::Environment);
                    for(std::size_t k=0;k<count&&ok;++k) {
                        const auto rule=k<s->arguments.size()?s->arguments[k]:*s->variadic;
                        ok=rule==H::ArgumentType::Numeric?arg(k)==H::Type::Real:
                           rule==H::ArgumentType::String?arg(k)==H::Type::String:valid_type(arg(k));
                    }
                }
                break;
            }
            default:ok=false;
            }
            if(!ok) report(i.origin,"MIR_TYPE","instruction type/signature/metadata mismatch");
        }
        const auto& t=*b.terminator;
        if(t.condition && (t.condition->value>=f.values.size()||!H::numeric(f.values[t.condition->value].type)))
            report(t.origin,"MIR_CONDITION","branch condition must be a valid numeric ValueId");
    }
    if(!ds.empty()) return ds;
    const auto analysis=analyze_dominance(f);
    if(analysis.has_errors()) {
        ds.insert(ds.end(),analysis.diagnostics.begin(),analysis.diagnostics.end());return ds;
    }
    const auto& dom=analysis.dominators;
    auto use=[&](ValueId value,std::size_t b,std::size_t k,H::Origin origin) {
        const auto d=*definitions[value.value];
        if((d.block==b&&d.index>=k)||(d.block!=b&&(!cfg.reachable[b]||!dom[b][d.block])))
            report(origin,"MIR_DOMINANCE","ValueId definition does not dominate its use");
    };
    for(std::size_t b=0;b<n;++b) {
        for(std::size_t k=0;k<f.blocks[b].instructions.size();++k) {
            for(const auto v:f.blocks[b].instructions[k].operands) use(v,b,k,f.blocks[b].instructions[k].origin);
            // phi 参数在前驱块的出口使用，而非在当前块的 phi 位置使用。
            for(const auto& input:f.blocks[b].instructions[k].incoming)
                use(input.value,input.predecessor.value,f.blocks[input.predecessor.value].instructions.size(),
                    f.blocks[b].instructions[k].origin);
        }
        const auto& t=*f.blocks[b].terminator;
        if(t.condition) use(*t.condition,b,f.blocks[b].instructions.size(),t.origin);
        for(const auto& output:t.outputs) use(output.value,b,f.blocks[b].instructions.size(),t.origin);
    }
    return ds;
}
} // namespace vagrad::mir
