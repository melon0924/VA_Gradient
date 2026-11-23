#include "vagrad/analysis/dominance.hpp"
#include <algorithm>
#include <ostream>
#include <set>

namespace vagrad::mir {
bool DominanceInfo::has_errors() const {return cfg.has_errors()||!diagnostics.empty();}
bool DominanceInfo::dominates(BlockId a,BlockId b) const {
    return b.value<dominators.size()&&a.value<dominators[b.value].size()&&dominators[b.value][a.value];
}
DominanceInfo analyze_dominance(const MirFunction& f) {
    DominanceInfo d;d.cfg=build_cfg(f);
    if(d.cfg.has_errors()) return d;
    const auto n=f.blocks.size(),entry=f.entry->value;
    if(n>2048) {
        d.diagnostics.push_back({Severity::Error,"MIR_LIMIT","dominance analysis exceeds 2048 blocks",{},{}});
        return d;
    }
    d.dominators.assign(n,std::vector<bool>(n));
    d.immediate_dominator.resize(n);d.tree_children.resize(n);d.frontier.resize(n);
    for(std::size_t b=0;b<n;++b) if(d.cfg.reachable[b]) {
        if(b==entry) d.dominators[b][b]=true;
        else d.dominators[b]=d.cfg.reachable;
    }
    bool changed=true;
    while(changed) {
        changed=false;
        for(const auto b:d.cfg.reverse_postorder) if(b.value!=entry) {
            auto next=d.cfg.reachable;
            for(const auto pred:d.cfg.predecessors[b.value]) if(d.cfg.reachable[pred.value])
                for(std::size_t k=0;k<n;++k) next[k]=next[k]&&d.dominators[pred.value][k];
            next[b.value]=true;
            if(next!=d.dominators[b.value]) {d.dominators[b.value]=std::move(next);changed=true;}
        }
    }
    // 一个块的严格支配者形成链，最深者就是直接支配者。
    std::vector<std::size_t> depth(n);
    for(std::size_t b=0;b<n;++b) depth[b]=static_cast<std::size_t>(
        std::count(d.dominators[b].begin(),d.dominators[b].end(),true));
    for(const auto b:d.cfg.reverse_postorder) if(b.value!=entry) {
        std::optional<BlockId> parent;
        for(std::size_t a=0;a<n;++a) if(a!=b.value&&d.dominators[b.value][a]&&
            (!parent||depth[a]>depth[parent->value])) parent=BlockId{a};
        d.immediate_dominator[b.value]=parent;
        if(parent) d.tree_children[parent->value].push_back(b);
    }
    std::vector<std::set<std::size_t>> frontier(n);
    for(const auto b:d.cfg.reverse_postorder) for(const auto pred:d.cfg.predecessors[b.value])
        if(d.cfg.reachable[pred.value]) {
            auto runner=std::optional<BlockId>{pred};
            while(runner&&runner!=d.immediate_dominator[b.value]) {
                frontier[runner->value].insert(b.value);
                runner=d.immediate_dominator[runner->value];
            }
        }
    for(std::size_t b=0;b<n;++b) {
        std::sort(d.tree_children[b].begin(),d.tree_children[b].end(),
            [](BlockId a,BlockId c){return a.value<c.value;});
        for(const auto k:frontier[b]) d.frontier[b].push_back({k});
    }
    return d;
}
void print_dominance(std::ostream& out,const DominanceInfo& d) {
    out<<"Dominance analysis\n";
    for(const auto& x:d.cfg.diagnostics) out<<"Diagnostic "<<x.code<<": "<<x.message<<'\n';
    for(const auto& x:d.diagnostics) out<<"Diagnostic "<<x.code<<": "<<x.message<<'\n';
    if(d.has_errors()) return;
    for(std::size_t b=0;b<d.dominators.size();++b) {
        out<<"bb"<<b<<" reachable="<<d.cfg.reachable[b]<<" idom=";
        if(d.immediate_dominator[b]) out<<"bb"<<d.immediate_dominator[b]->value;
        else out<<"<none>";
        out<<" frontier=[";
        for(const auto a:d.frontier[b]) out<<" bb"<<a.value;
        out<<" ] children=[";
        for(const auto a:d.tree_children[b]) out<<" bb"<<a.value;
        out<<" ]\n";
    }
}
} // namespace vagrad::mir
