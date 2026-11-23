#pragma once
#include "vagrad/ir/mir/mir.hpp"
#include "vagrad/frontend/hir/eval.hpp"

namespace vagrad::mir {
// 执行已校验的 MIR 基本块；数值结果使用与 H3 相同的观察接口。
struct ExecutionObserver {
    virtual ~ExecutionObserver()=default;
    virtual void phi(const std::vector<Instruction>&,BlockId) {}
    virtual void instruction(const Instruction&,const std::vector<hir::Scalar>&,const std::optional<hir::Scalar>&) {}
    virtual void branch(const Terminator&,const hir::Scalar&) {}
    virtual void returned(const Terminator&,const std::vector<std::optional<hir::Scalar>>&) {}
};
// 构造时复制并校验。只读快照允许多次执行跳过结构/支配关系重算。
class Executable {
public:
    explicit Executable(const MirFunction&);
    const MirFunction& function() const {return *function_;}
    const std::vector<Diagnostic>& diagnostics() const {return diagnostics_;}
    bool has_errors() const {return !diagnostics_.empty();}
private:
    std::shared_ptr<const MirFunction> function_;
    std::vector<Diagnostic> diagnostics_;
};
hir::EvalResult evaluate_mir(const MirFunction&,const hir::ParamEnv&,const hir::EvalInputs&,ExecutionObserver* observer=nullptr);
hir::EvalResult evaluate_mir(const Executable&,const hir::ParamEnv&,const hir::EvalInputs&,ExecutionObserver* observer=nullptr);
} // namespace vagrad::mir
