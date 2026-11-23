#pragma once
#include "vagrad/ad/sparsity.hpp"

namespace vagrad::ad {
// SSA 执行适配器的导数存储。空值保留未赋值状态；不是零梯度。
class TangentState {
public:
    TangentState(std::size_t values,std::size_t lanes);
    TangentState(const std::vector<LaneMask>& masks,std::size_t lanes);
    // 计算规则仍接收固定长度向量；持久存储只保存掩码中的分量。
    std::optional<std::vector<double>> get(mir::ValueId id) const;
    void set(mir::ValueId,std::optional<std::vector<double>>);
    // 仅对已校验 SSA 的块首 phi 调用；按实际前驱同时更新。
    void apply_phi(const std::vector<mir::Instruction>&,mir::BlockId predecessor);
private:
    std::vector<std::optional<std::vector<double>>> values_;
    std::vector<LaneMask> masks_;
    std::size_t lanes_;
};
} // namespace vagrad::ad
