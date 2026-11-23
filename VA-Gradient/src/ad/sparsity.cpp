#include "vagrad/ad/sparsity.hpp"
#include <algorithm>
#include <bit>
#include <deque>
#include <limits>
#include <ostream>

namespace vagrad::ad {
namespace {
// OR 是单调操作；队列到达固定点时，回边与 phi 也已覆盖。
void propagate(std::vector<LaneMask>& masks,const std::vector<std::vector<std::size_t>>& users) {
    std::deque<std::size_t> work;
    std::vector<bool> queued(masks.size());
    for(std::size_t k=0;k<masks.size();++k) if(masks[k]) {work.push_back(k);queued[k]=true;}
    while(!work.empty()) {
        const auto id=work.front();work.pop_front();queued[id]=false;
        for(const auto user:users[id]) {
            const auto merged=masks[user]|masks[id];
            if(merged==masks[user]) continue;
            masks[user]=merged;
            if(!queued[user]) {work.push_back(user);queued[user]=true;}
        }
    }
}
}
Sparsity analyze_sparsity(const mir::MirFunction& f,const Plan& p,const hir::ParamEnv& env) {
    Sparsity r;r.lanes=p.parameters.size();
    r.diagnostics=mir::verify_mir(f);
    if(r.has_errors()) return r;
    if(p.has_errors()||p.hir!=f.hir||f.form!=mir::MirForm::Ssa||r.lanes==0||r.lanes>64||
       env.has_errors()||env.bindings!=f.hir->bindings) {
        r.diagnostics.push_back({Severity::Error,"AD_PLAN","sparsity requires a valid SSA, seed plan and parameter environment",{},{}});
        return r;
    }
    std::vector<LaneMask> params(f.hir->bindings->symbols.size());
    for(std::size_t k=0;k<p.parameters.size();++k) params[p.parameters[k].value]=LaneMask{1}<<k;
    if(p.mode==ParameterMode::Defaults) {
        std::vector<std::vector<std::size_t>> users(params.size());
        std::vector<std::size_t> visited(f.hir->expressions.size(),std::numeric_limits<std::size_t>::max());
        for(const auto& parameter:f.hir->parameters) {
            const auto id=parameter.symbol;
            // 种子自身独立；显式覆盖的非种子参数冻结。
            if(params[id.value]||std::find(env.overridden.begin(),env.overridden.end(),id)!=env.overridden.end()||
               !parameter.default_value) continue;
            std::vector<hir::ExprId> work{*parameter.default_value};
            while(!work.empty()) {
                const auto expression=work.back();work.pop_back();
                if(visited[expression.value]==id.value) continue;
                visited[expression.value]=id.value;
                const auto& e=f.hir->expr(expression);
                if(e.kind==hir::ExprKind::Ref) users[e.symbol->value].push_back(id.value);
                for(const auto operand:e.operands) work.push_back(operand);
            }
        }
        propagate(params,users);
    }
    r.values.resize(f.values.size());
    std::vector<std::vector<std::size_t>> users(f.values.size());
    for(const auto& b:f.blocks) for(const auto& i:b.instructions) if(i.result) {
        const auto id=i.result->value;
        if(i.opcode==mir::Opcode::LoadParam) r.values[id]=params[i.parameter->value];
        for(const auto v:i.operands) users[v.value].push_back(id);
        for(const auto& edge:i.incoming) users[edge.value.value].push_back(id);
    }
    propagate(r.values,users);
    r.dense_slots=r.values.size()*r.lanes;
    for(const auto mask:r.values) r.sparse_slots+=static_cast<std::size_t>(std::popcount(mask));
    return r;
}
void print_sparsity(std::ostream& out,const Sparsity& s,const Plan& p) {
    out<<"AD seed support: slots="<<s.sparse_slots<<'/'<<s.dense_slots<<'\n';
    for(std::size_t id=0;id<s.values.size();++id) {
        out<<"  v"<<id<<" :";
        for(std::size_t lane=0;lane<s.lanes;++lane) if(s.values[id]&(LaneMask{1}<<lane))
            out<<' '<<p.hir->bindings->symbol(p.parameters[lane]).name;
        out<<'\n';
    }
}
} // namespace vagrad::ad
