#pragma once
#include "vagrad/ad/eval.hpp"

namespace vagrad::codegen {
struct CSource {
    std::string text;
    std::vector<Diagnostic> diagnostics;
    bool has_errors() const {return !diagnostics.empty();}
};
// C0：独立有效参数、静态标量 SSA。产物自带数值/导数辅助函数，不依赖本项目头文件。
CSource emit_c(const ad::Prepared&,const hir::ParamEnv& default_effective_parameters);
} // namespace vagrad::codegen
