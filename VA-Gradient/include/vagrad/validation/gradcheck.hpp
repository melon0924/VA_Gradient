#pragma once
#include "vagrad/ad/eval.hpp"

namespace vagrad::validation {
struct GradientOptions {
    std::map<std::string,double> steps;
    double relative_step=1e-5,absolute_step=1e-12;
    double relative_tolerance=1e-5,absolute_tolerance=1e-10;
};
struct GradientRow {
    std::string output,seed;
    double automatic=0,numerical=0,step=0,error=0,tolerance=0;
    bool passed=false;
};
struct GradientCheck {
    ad::Result automatic;
    std::vector<GradientRow> rows;
    std::vector<Diagnostic> diagnostics;
    bool has_errors() const {return automatic.has_errors()||!diagnostics.empty();}
    bool passed() const;
};
// 同参数模式的中心差分。跨路径、范围失败或输入问题是未完成核验，不能标成通过。
GradientCheck check_gradients(const mir::MirFunction&,const hir::ParamEnv&,const hir::EvalInputs&,
    const std::vector<std::string>& seeds,const GradientOptions& options={},
    ad::ParameterMode mode=ad::ParameterMode::Effective);
void print_gradient_check(std::ostream&,const GradientCheck&);
} // namespace vagrad::validation
