#pragma once
#include "vagrad/frontend/hir/eval.hpp"

namespace vagrad::runtime {
inline constexpr double thermal_voltage_per_kelvin=8.617333262145e-5;
struct Stop {};
// HIR 与 MIR 共用数值原语和域检查，各自保留独立的控制流执行方式。
class ScalarOps {
public:
    ScalarOps(const hir::HirModule& module,std::vector<Diagnostic>& diagnostics);
    const hir::HirModule& m;
    std::vector<Diagnostic>& diagnostics;
    const hir::EvalInputs* inputs=nullptr;
    std::size_t steps=0,limit=100000;
    [[noreturn]] void fail(hir::Origin,std::string code,std::string message);
    void tick(hir::Origin);
    hir::Scalar finite(hir::Scalar,hir::Origin);
    hir::Scalar integer(double,hir::Origin,bool round=true);
    hir::Scalar convert(hir::Scalar,hir::Type,hir::Origin);
    double temperature(hir::Origin);
    double voltage(std::optional<hir::SymbolId>,hir::Origin);
    hir::Scalar unary(std::string_view,hir::Type,const hir::Scalar&,hir::Origin);
    hir::Scalar binary(std::string_view,hir::Type,const hir::Scalar&,const hir::Scalar&,hir::Origin);
    hir::Scalar math(std::string_view,const std::vector<hir::Scalar>&,hir::Origin);
};
} // namespace vagrad::runtime
