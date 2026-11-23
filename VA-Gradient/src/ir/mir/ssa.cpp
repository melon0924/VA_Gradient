#include "vagrad/ir/mir/mir.hpp"
#include "vagrad/analysis/dominance.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace vagrad::mir {
namespace {
class Promoter {
public:
    explicit Promoter(const MirFunction& input):f_(input) {}
    MirFunction run() {
        const auto errors=verify_mir(f_);
        if(!errors.empty()) {f_.diagnostics.insert(f_.diagnostics.end(),errors.begin(),errors.end());return std::move(f_);}
        if(f_.form!=MirForm::Local) {fail("SSA_FORM","promotion requires MIR-Local");return std::move(f_);}
        if(f_.blocks.size()*f_.locals.size()>8000000) {
            fail("SSA_LIMIT","SSA liveness matrix exceeds 8000000 entries");return std::move(f_);
        }
        // 带回边的入口先增加前置块，初次进入也就有明确的 phi 前驱。
        auto cfg=build_cfg(f_);
        if(!cfg.predecessors[f_.entry->value].empty()) {
            const auto old=*f_.entry;
            const auto origin=f_.blocks[old.value].terminator->origin;
            f_.entry=BlockId{f_.blocks.size()};
            f_.blocks.push_back({{},Terminator{TerminatorKind::Jump,origin,{},old,{}}});
        }
        const auto analysis=analyze_dominance(f_);
        if(analysis.has_errors()) {fail("SSA_CFG","dominance analysis failed");return std::move(f_);}
        const auto n=f_.blocks.size(),slots=f_.locals.size();
        std::vector<std::vector<bool>> uses(n,std::vector<bool>(slots)),defs=uses,live_in=uses,live_out=uses;
        std::vector<std::set<std::size_t>> definition_blocks(slots);
        std::vector<bool> observable(slots);
        for(std::size_t l=0;l<slots;++l)
            observable[l]=f_.locals[l].symbol&&f_.hir->bindings->symbol(*f_.locals[l].symbol).scope==f_.hir->scope;
        for(std::size_t b=0;b<n;++b) {
            if(!analysis.cfg.reachable[b]) {
                // 已校验的不可达块只有 Unreachable；明确丢弃其中不可执行的普通指令。
                f_.blocks[b].instructions.clear();continue;
            }
            for(const auto& i:f_.blocks[b].instructions) {
                if(i.opcode==Opcode::LoadLocal&&!defs[b][i.local->value]) uses[b][i.local->value]=true;
                if(i.opcode==Opcode::StoreLocal) {
                    defs[b][i.local->value]=true;definition_blocks[i.local->value].insert(b);
                }
            }
            if(f_.blocks[b].terminator->kind==TerminatorKind::Return)
                for(std::size_t l=0;l<slots;++l) if(observable[l]&&!defs[b][l]) uses[b][l]=true;
        }
        bool changed=true;
        while(changed) {
            changed=false;
            for(auto it=analysis.cfg.reverse_postorder.rbegin();it!=analysis.cfg.reverse_postorder.rend();++it) {
                const auto b=it->value;
                std::vector<bool> out(slots),in=uses[b];
                for(const auto s:analysis.cfg.successors[b])
                    for(std::size_t l=0;l<slots;++l) out[l]=out[l]||live_in[s.value][l];
                for(std::size_t l=0;l<slots;++l) in[l]=in[l]||(out[l]&&!defs[b][l]);
                if(in!=live_in[b]||out!=live_out[b]) {live_in[b]=std::move(in);live_out[b]=std::move(out);changed=true;}
            }
        }
        std::vector<std::vector<LocalId>> phi_slots(n);
        for(std::size_t l=0;l<slots;++l) {
            std::vector<std::size_t> work(definition_blocks[l].begin(),definition_blocks[l].end());
            std::vector<bool> placed(n);
            while(!work.empty()) {
                const auto b=work.back();work.pop_back();
                for(const auto y:analysis.frontier[b]) if(live_in[y.value][l]&&!placed[y.value]) {
                    placed[y.value]=true;phi_slots[y.value].push_back({l});
                    if(!definition_blocks[l].contains(y.value)) work.push_back(y.value);
                }
            }
        }
        std::size_t added=slots;
        for(const auto& list:phi_slots) added+=list.size();
        if(f_.values.size()+added>1000000) {fail("SSA_LIMIT","SSA value arena exceeds 1000000 entries");return std::move(f_);}
        f_.form=MirForm::Ssa;
        for(std::size_t b=0;b<n;++b) {
            std::vector<Instruction> instructions;
            for(const auto local:phi_slots[b]) {
                Instruction phi;phi.opcode=Opcode::Phi;phi.origin=f_.locals[local.value].origin;
                phi.local=local;phi.result=new_value(f_.locals[local.value].type,phi.origin);
                instructions.push_back(std::move(phi));
            }
            auto& old=f_.blocks[b].instructions;
            instructions.insert(instructions.end(),std::make_move_iterator(old.begin()),std::make_move_iterator(old.end()));
            old=std::move(instructions);
        }
        // Undef 表示本次求值尚未赋值；它不是数值 0，也不是优化器任意选值。
        std::vector<std::vector<ValueId>> names(slots);
        std::vector<Instruction> initial;
        for(std::size_t l=0;l<slots;++l) {
            Instruction undef;undef.opcode=Opcode::Undef;undef.origin=f_.locals[l].origin;undef.local=LocalId{l};
            undef.result=new_value(f_.locals[l].type,undef.origin);
            names[l].push_back(*undef.result);initial.push_back(std::move(undef));
        }
        auto& entry=f_.blocks[f_.entry->value].instructions;
        initial.insert(initial.end(),std::make_move_iterator(entry.begin()),std::make_move_iterator(entry.end()));
        entry=std::move(initial);
        aliases_.reserve(f_.values.size());
        for(std::size_t v=0;v<f_.values.size();++v) aliases_.push_back({v});
        struct Frame {BlockId block;std::size_t next=0;std::vector<LocalId> pushed;};
        auto enter=[&](BlockId id) {
            Frame frame{id};
            std::vector<Instruction> rewritten;
            for(auto i: f_.blocks[id.value].instructions) {
                for(auto& v:i.operands) v=resolve(v);
                if(i.opcode==Opcode::Phi) {
                    names[i.local->value].push_back(*i.result);frame.pushed.push_back(*i.local);
                } else if(i.opcode==Opcode::StoreLocal) {
                    names[i.local->value].push_back(i.operands[0]);frame.pushed.push_back(*i.local);continue;
                } else if(i.opcode==Opcode::LoadLocal) {
                    const auto value=names[i.local->value].back();
                    aliases_[i.result->value]=value;
                    // 保留原读取位置的未赋值检查，连被丢弃的独立表达式读取也不能消失。
                    i.opcode=Opcode::RequireDefined;i.operands={value};i.result.reset();
                }
                rewritten.push_back(std::move(i));
            }
            f_.blocks[id.value].instructions=std::move(rewritten);
            auto& t=*f_.blocks[id.value].terminator;
            if(t.condition) t.condition=resolve(*t.condition);
            if(t.kind==TerminatorKind::Return)
                for(std::size_t l=0;l<slots;++l) if(observable[l])
                    t.outputs.push_back({*f_.locals[l].symbol,resolve(names[l].back())});
            for(const auto successor:analysis.cfg.successors[id.value])
                for(auto& phi:f_.blocks[successor.value].instructions) {
                    if(phi.opcode!=Opcode::Phi) break;
                    phi.incoming.push_back({id,resolve(names[phi.local->value].back())});
                }
            return frame;
        };
        std::vector<Frame> stack;stack.push_back(enter(*f_.entry));
        while(!stack.empty()) {
            auto& frame=stack.back();
            const auto& children=analysis.tree_children[frame.block.value];
            if(frame.next<children.size()) {
                const auto child=children[frame.next++];stack.push_back(enter(child));
            } else {
                for(auto it=frame.pushed.rbegin();it!=frame.pushed.rend();++it) names[it->value].pop_back();
                stack.pop_back();
            }
        }
        // 只折叠输入实际相同（允许自引用）的冗余 phi，不把 Undef 与普通数值合并。
        changed=true;
        while(changed) {
            changed=false;
            for(auto& b:f_.blocks) for(auto& i:b.instructions) if(i.opcode==Opcode::Phi&&resolve(*i.result)==*i.result) {
                std::optional<ValueId> same;bool different=false;
                for(const auto& input:i.incoming) {
                    const auto value=resolve(input.value);
                    if(value==*i.result) continue;
                    if(same&&*same!=value) different=true;
                    else same=value;
                }
                if(!different&&same) {aliases_[i.result->value]=*same;changed=true;}
            }
        }
        for(auto& b:f_.blocks) {
            std::erase_if(b.instructions,[&](const auto& i){return i.opcode==Opcode::Phi&&resolve(*i.result)!=*i.result;});
            for(auto& i:b.instructions) {
                for(auto& v:i.operands) v=resolve(v);
                for(auto& input:i.incoming) input.value=resolve(input.value);
                std::sort(i.incoming.begin(),i.incoming.end(),[](const auto& a,const auto& c){
                    return a.predecessor.value<c.predecessor.value;});
            }
            if(b.terminator->condition) b.terminator->condition=resolve(*b.terminator->condition);
            for(auto& output:b.terminator->outputs) output.value=resolve(output.value);
        }
        compact_values();
        const auto final_errors=verify_mir(f_);
        f_.diagnostics.insert(f_.diagnostics.end(),final_errors.begin(),final_errors.end());
        return std::move(f_);
    }
private:
    MirFunction f_;
    std::vector<ValueId> aliases_;
    void fail(std::string code,std::string message) {
        f_.diagnostics.push_back({Severity::Error,std::move(code),std::move(message),{},{}});
    }
    ValueId new_value(hir::Type type,hir::Origin origin) {
        const ValueId id{f_.values.size()};f_.values.push_back({type,origin});return id;
    }
    ValueId resolve(ValueId v) {
        auto root=v;
        while(aliases_[root.value]!=root) root=aliases_[root.value];
        while(aliases_[v.value]!=v) {const auto next=aliases_[v.value];aliases_[v.value]=root;v=next;}
        return root;
    }
    void compact_values() {
        std::vector<std::optional<ValueId>> map(f_.values.size());
        std::vector<ValueInfo> values;
        for(const auto& b:f_.blocks) for(const auto& i:b.instructions) if(i.result) {
            map[i.result->value]=ValueId{values.size()};values.push_back(f_.values[i.result->value]);
        }
        auto remap=[&](ValueId v) {
            if(!map[v.value]) throw std::logic_error("SSA references a removed definition");
            return *map[v.value];
        };
        for(auto& b:f_.blocks) {
            for(auto& i:b.instructions) {
                if(i.result) i.result=remap(*i.result);
                for(auto& v:i.operands) v=remap(v);
                for(auto& input:i.incoming) input.value=remap(input.value);
            }
            if(b.terminator->condition) b.terminator->condition=remap(*b.terminator->condition);
            for(auto& output:b.terminator->outputs) output.value=remap(output.value);
        }
        f_.values=std::move(values);
    }
};
}
MirFunction promote_to_ssa(const MirFunction& input) {return Promoter(input).run();}
} // namespace vagrad::mir
