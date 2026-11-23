#include "vagrad/ir/mir/slice.hpp"
#include <algorithm>
#include <limits>
#include <ostream>
#include <set>

namespace vagrad::mir {
namespace {
namespace H=hir;
using Position=std::pair<std::size_t,std::size_t>;
std::pair<std::size_t,std::size_t> key(const H::Probe& p) {
    const auto a=p.positive?p.positive->value:std::numeric_limits<std::size_t>::max();
    const auto b=p.negative?p.negative->value:std::numeric_limits<std::size_t>::max();
    return {std::min(a,b),std::max(a,b)};
}
bool effect(Opcode op) {return op==Opcode::DiagnosticCall||op==Opcode::ContributeFlow||op==Opcode::RequireDefined;}
struct Mark {
    std::vector<std::vector<bool>> keep;
    std::vector<bool> values;
    std::vector<Position> work;
};
}
bool SliceResult::has_errors() const {return function.has_errors()||!diagnostics.empty();}
SliceResult slice_ssa(const MirFunction& source,std::string target,SliceEffects effects) {
    SliceResult result(source);result.target=std::move(target);result.effects=effects;
    auto fail=[&](std::string code,std::string message) {
        result.diagnostics.push_back({Severity::Error,std::move(code),std::move(message),{},{}});
    };
    const auto errors=verify_mir(source);
    if(!errors.empty()) {result.diagnostics=errors;return result;}
    if(source.form!=MirForm::Ssa||source.output_projection||source.flow_projection) {
        fail("SLICE_INPUT","slicing requires a full, verified SSA function");return result;
    }
    const auto& h=*source.hir;const auto cfg=build_cfg(source);const auto n=source.blocks.size();
    std::optional<H::SymbolId> variable,branch;std::optional<H::Probe> branch_probe;
    if(result.target.starts_with("flow(")&&result.target.ends_with(")@exit")) {
        const auto name=result.target.substr(5,result.target.size()-11);
        for(const auto& b:h.branches) if(h.bindings->symbol(b.symbol).name==name) {branch=b.symbol;branch_probe=b.probe;}
    } else if(result.target.ends_with("@exit")) {
        const auto name=result.target.substr(0,result.target.size()-5);
        for(const auto& v:h.variables) if(h.bindings->symbol(v.symbol).scope==h.scope&&h.bindings->symbol(v.symbol).name==name)
            variable=v.symbol;
    }
    if(!variable&&!branch) {fail("SLICE_TARGET","expected a module variable@exit or flow(named_branch)@exit");return result;}
    std::vector<Position> definitions(source.values.size());
    Mark core;core.values.resize(source.values.size());core.keep.resize(n);
    for(std::size_t b=0;b<n;++b) {
        core.keep[b].resize(source.blocks[b].instructions.size());
        result.original_instructions+=source.blocks[b].instructions.size();
        for(std::size_t k=0;k<source.blocks[b].instructions.size();++k)
            if(const auto v=source.blocks[b].instructions[k].result) definitions[v->value]={b,k};
    }
    // CFG 保留原结构。用后支配关系求控制依赖；无法到达出口的区域采用保守策略。
    std::vector<bool> reaches_exit(n);
    std::vector<std::size_t> work;
    for(std::size_t b=0;b<n;++b) if(cfg.reachable[b]&&cfg.successors[b].empty()) {reaches_exit[b]=true;work.push_back(b);}
    while(!work.empty()) {
        const auto b=work.back();work.pop_back();
        for(const auto p:cfg.predecessors[b]) if(!reaches_exit[p.value]) {reaches_exit[p.value]=true;work.push_back(p.value);}
    }
    std::vector<std::vector<BlockId>> controls(n);
    for(std::size_t b=0;b<n;++b) if(cfg.reachable[b]&&!reaches_exit[b]) result.conservative_control=true;
    if(result.conservative_control) {
        for(std::size_t x=0;x<n;++x) if(cfg.reachable[x])
            for(std::size_t b=0;b<n;++b) if(cfg.reachable[b]&&source.blocks[b].terminator->kind==TerminatorKind::Branch)
                controls[x].push_back({b});
    } else {
        std::vector<std::vector<bool>> post(n+1,std::vector<bool>(n+1));
        post[n][n]=true;
        for(std::size_t b=0;b<n;++b) if(cfg.reachable[b]) {post[b][n]=true;for(std::size_t x=0;x<n;++x) post[b][x]=cfg.reachable[x];}
        bool changed=true;
        while(changed) {
            changed=false;
            for(auto it=cfg.reverse_postorder.rbegin();it!=cfg.reverse_postorder.rend();++it) {
                const auto b=it->value;std::vector<bool> next(n+1,true);
                if(cfg.successors[b].empty()) next=post[n];
                else for(const auto s:cfg.successors[b])
                    for(std::size_t x=0;x<=n;++x) next[x]=next[x]&&post[s.value][x];
                next[b]=true;
                if(next!=post[b]) {post[b]=std::move(next);changed=true;}
            }
        }
        for(std::size_t b=0;b<n;++b) if(cfg.reachable[b]&&source.blocks[b].terminator->kind==TerminatorKind::Branch)
            for(std::size_t x=0;x<n;++x) if(cfg.reachable[x]&&(x==b||!post[b][x])) {
                bool selected=false;
                for(const auto s:cfg.successors[b]) selected=selected||post[s.value][x];
                if(selected) controls[x].push_back({b});
            }
    }
    std::vector<std::size_t> indegree(n);std::size_t reachable=0,visited=0;
    for(std::size_t b=0;b<n;++b) if(cfg.reachable[b]) {
        ++reachable;indegree[b]=cfg.predecessors[b].size();if(indegree[b]==0) work.push_back(b);
    }
    while(!work.empty()) {
        const auto b=work.back();work.pop_back();++visited;
        for(const auto s:cfg.successors[b]) if(--indegree[s.value]==0) work.push_back(s.value);
    }
    result.cfg_has_cycle=visited!=reachable;
    auto mark_instruction=[&](Mark& m,Position p) {
        if(!m.keep[p.first][p.second]) {
            m.keep[p.first][p.second]=true;m.work.push_back(p);
            if(const auto v=source.blocks[p.first].instructions[p.second].result) m.values[v->value]=true;
        }
    };
    auto mark_value=[&](Mark& m,ValueId v) {mark_instruction(m,definitions[v.value]);};
    auto mark_control=[&](Mark& m,std::size_t b) {
        for(const auto controller:controls[b]) mark_value(m,*source.blocks[controller.value].terminator->condition);
    };
    auto drain=[&](Mark& m) {
        while(!m.work.empty()) {
            const auto [b,k]=m.work.back();m.work.pop_back();const auto& i=source.blocks[b].instructions[k];
            mark_control(m,b);
            for(const auto v:i.operands) mark_value(m,v);
            for(const auto& input:i.incoming) {mark_value(m,input.value);mark_control(m,input.predecessor.value);}
        }
    };
    std::size_t roots=0;
    for(std::size_t b=0;b<n;++b) if(cfg.reachable[b]) {
        if(variable) for(const auto& output:source.blocks[b].terminator->outputs) if(output.symbol==*variable) {
            mark_value(core,output.value);mark_control(core,b);++roots;
        }
        if(branch) for(std::size_t k=0;k<source.blocks[b].instructions.size();++k) {
            const auto& i=source.blocks[b].instructions[k];
            if(i.opcode==Opcode::ContributeFlow&&key(*i.probe)==key(*branch_probe)) {mark_instruction(core,{b,k});++roots;}
        }
    }
    if(variable&&roots==0) {fail("SLICE_TARGET","variable has no reachable Return observation");return result;}
    drain(core);
    Mark execution=core;
    for(std::size_t b=0;b<n;++b) if(cfg.reachable[b]) {
        if(const auto c=source.blocks[b].terminator->condition) mark_value(execution,*c);
        if(effects==SliceEffects::Preserve)
            for(std::size_t k=0;k<source.blocks[b].instructions.size();++k) {
                const auto op=source.blocks[b].instructions[k].opcode;
                // 所有可能读取输入、报错或产生副作用的指令都保留。
                if(op!=Opcode::Undef&&op!=Opcode::Phi) mark_instruction(execution,{b,k});
            }
    }
    drain(execution);
    auto inputs=[&](const Mark& m) {
        SliceInputs found;std::set<std::size_t> parameters,nodes;
        for(std::size_t b=0;b<n;++b) for(std::size_t k=0;k<m.keep[b].size();++k) if(m.keep[b][k]) {
            const auto& i=source.blocks[b].instructions[k];
            if(i.parameter) parameters.insert(i.parameter->value);
            if(i.opcode==Opcode::Temperature||(i.opcode==Opcode::MathCall&&i.op=="$vt"&&i.operands.empty())) found.temperature=true;
            if(i.opcode==Opcode::ReadPotential) {
                found.potentials.push_back(*i.probe);
                if(i.probe->positive) nodes.insert(i.probe->positive->value);
                if(i.probe->negative) nodes.insert(i.probe->negative->value);
            }
        }
        for(const auto p:parameters) found.parameters.push_back({p});
        for(const auto p:nodes) found.nodes.push_back({p});
        return found;
    };
    result.target_inputs=inputs(core);result.execution_inputs=inputs(execution);
    auto& f=result.function;
    f.output_projection=variable?std::vector<H::SymbolId>{*variable}:std::vector<H::SymbolId>{};
    if(effects==SliceEffects::TargetOnly) f.flow_projection=branch?std::vector<H::SymbolId>{*branch}:std::vector<H::SymbolId>{};
    for(std::size_t b=0;b<n;++b) {
        std::vector<Instruction> kept;
        for(std::size_t k=0;k<source.blocks[b].instructions.size();++k) {
            const auto& i=source.blocks[b].instructions[k];
            if(effect(i.opcode)) result.effect_decisions.push_back({{b},k,i.opcode,i.origin,execution.keep[b][k]});
            if(execution.keep[b][k]) kept.push_back(i);
        }
        result.retained_instructions+=kept.size();f.blocks[b].instructions=std::move(kept);
        std::erase_if(f.blocks[b].terminator->outputs,[&](const auto& o){return !variable||o.symbol!=*variable;});
    }
    std::vector<std::optional<ValueId>> remap(f.values.size());std::vector<ValueInfo> values;
    for(const auto& b:f.blocks) for(const auto& i:b.instructions) if(i.result) {
        remap[i.result->value]=ValueId{values.size()};values.push_back(f.values[i.result->value]);
    }
    auto value=[&](ValueId v) {return *remap[v.value];};
    for(auto& b:f.blocks) {
        for(auto& i:b.instructions) {
            if(i.result) i.result=value(*i.result);
            for(auto& v:i.operands) v=value(v);
            for(auto& input:i.incoming) input.value=value(input.value);
        }
        if(b.terminator->condition) b.terminator->condition=value(*b.terminator->condition);
        for(auto& output:b.terminator->outputs) output.value=value(output.value);
    }
    f.values=std::move(values);
    result.diagnostics=verify_mir(f);
    return result;
}
void print_slice(std::ostream& out,const SliceResult& r) {
    out<<"SSA slice target="<<r.target<<" effects="<<(r.effects==SliceEffects::Preserve?"preserve":"target-only")
       <<" instructions="<<r.retained_instructions<<'/'<<r.original_instructions
       <<" cfg_cycle="<<r.cfg_has_cycle<<" conservative_control="<<r.conservative_control<<'\n';
    for(const auto& d:r.diagnostics) out<<"Diagnostic "<<d.code<<": "<<d.message<<'\n';
    if(r.has_errors()) return;
    const auto& h=*r.function.hir;
    auto inputs=[&](std::string_view label,const SliceInputs& i) {
        out<<label<<" parameters=[";
        for(const auto p:i.parameters) out<<' '<<h.bindings->symbol(p).name<<':'<<H::type_name(h.symbol_types[p.value]);
        out<<" ] nodes=[";
        for(const auto node:i.nodes) out<<' '<<h.bindings->symbol(node).name;
        out<<" ] temperature="<<i.temperature<<'\n';
    };
    inputs("target_inputs",r.target_inputs);inputs("execution_inputs",r.execution_inputs);
    for(const auto& e:r.effect_decisions) out<<"Effect "<<opcode_name(e.opcode)<<" bb"<<e.block.value
        <<" instruction="<<e.instruction<<" retained="<<e.retained<<" token="<<e.origin.token_index<<'\n';
    out<<"CFG is preserved; no global straight-line evaluation order is implied.\n";
    print_mir(out,r.function);
}
} // namespace vagrad::mir
