#pragma once
#include "vagrad/ad/ir.hpp"
#include "vagrad/ad/sparsity.hpp"

namespace vagrad::ad {
// 不可变、可复用的编译快照。每次执行的值/导数/错误均在调用栈内新建。
class Prepared {
public:
    const mir::MirFunction& function() const {return executable_->function();}
    const mir::Executable& executable() const {return *executable_;}
    const Plan& plan() const {return plan_;}
    const DerivativeIr& derivatives() const {return derivatives_;}
    const Sparsity& sparsity() const {return sparsity_;}
    const std::vector<std::string>& seeds() const {return seeds_;}
    const std::vector<Diagnostic>& diagnostics() const {return diagnostics_;}
    bool has_errors() const {return !diagnostics_.empty();}
    bool accepts(const hir::ParamEnv&) const;
private:
    Prepared()=default;
    friend Prepared prepare(const mir::MirFunction&,const hir::ParamEnv&,const std::vector<std::string>&,ParameterMode);
    std::shared_ptr<const mir::Executable> executable_;
    Plan plan_;
    DerivativeIr derivatives_;
    Sparsity sparsity_;
    std::vector<std::string> seeds_;
    std::vector<hir::SymbolId> frozen_overrides_;
    std::vector<Diagnostic> diagnostics_;
};
Prepared prepare(const mir::MirFunction&,const hir::ParamEnv&,const std::vector<std::string>& seeds,
                 ParameterMode mode=ParameterMode::Effective);
} // namespace vagrad::ad
