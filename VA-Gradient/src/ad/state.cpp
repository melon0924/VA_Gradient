#include "vagrad/ad/state.hpp"
#include <algorithm>
#include <cmath>

namespace vagrad::ad {
namespace {
LaneMask all_lanes(std::size_t lanes) {
    if(lanes==0||lanes>64) throw Error("invalid AD lane count");
    return lanes==64?~LaneMask{0}:(LaneMask{1}<<lanes)-1;
}
}
TangentState::TangentState(std::size_t values,std::size_t lanes)
    :TangentState(std::vector<LaneMask>(values,all_lanes(lanes)),lanes) {}
TangentState::TangentState(const std::vector<LaneMask>& masks,std::size_t lanes)
    :values_(masks.size()),masks_(masks),lanes_(lanes) {
    const auto valid=all_lanes(lanes);
    for(const auto mask:masks_) if(mask&~valid) throw Error("invalid sparse AD mask");
}
std::optional<std::vector<double>> TangentState::get(mir::ValueId id) const {
    if(id.value>=values_.size()) throw Error("invalid AD ValueId");
    if(!values_[id.value]) return {};
    std::vector<double> dense(lanes_);std::size_t slot=0;
    for(std::size_t lane=0;lane<lanes_;++lane) if(masks_[id.value]&(LaneMask{1}<<lane))
        dense[lane]=values_[id.value]->at(slot++);
    return dense;
}
void TangentState::set(mir::ValueId id,std::optional<std::vector<double>> tangent) {
    if(id.value>=values_.size()) throw Error("invalid AD ValueId");
    if(tangent) {
        if(tangent->size()!=lanes_) throw Error("AD tangent lane count mismatch");
        for(const auto d:*tangent) if(!std::isfinite(d)) throw Error("non-finite AD tangent");
    }
    if(!tangent) {values_[id.value].reset();return;}
    std::vector<double> sparse;
    for(std::size_t lane=0;lane<lanes_;++lane) {
        if(masks_[id.value]&(LaneMask{1}<<lane)) sparse.push_back((*tangent)[lane]);
        else if((*tangent)[lane]!=0) throw Error("sparse AD support omitted a non-zero derivative");
    }
    values_[id.value]=std::move(sparse);
}
void TangentState::apply_phi(const std::vector<mir::Instruction>& instructions,mir::BlockId previous) {
    std::vector<std::pair<mir::ValueId,std::optional<std::vector<double>>>> pending;
    for(const auto& i:instructions) {
        if(i.opcode!=mir::Opcode::Phi) break;
        const auto input=std::find_if(i.incoming.begin(),i.incoming.end(),[&](const auto& edge){return edge.predecessor==previous;});
        if(!i.result||input==i.incoming.end()) throw Error("AD phi has no matching predecessor");
        pending.push_back({*i.result,get(input->value)});
    }
    for(auto& [id,tangent]:pending) set(id,std::move(tangent));
}
} // namespace vagrad::ad
