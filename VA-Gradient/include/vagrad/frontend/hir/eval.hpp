#pragma once

#include "vagrad/frontend/hir/typed.hpp"

namespace vagrad::hir {

struct ParamEnv {
    std::shared_ptr<const BindingResult> bindings; // 防止环境误用于另一个模型
    // 默认环境与本次有效环境分别计算。覆盖不会改写 HIR 中的 Ref。
    std::vector<std::optional<Scalar>> values; // SymbolId 索引
    std::vector<SymbolId> overridden; // 实际覆盖的参数；默认值链 AD 据此保留外部固定值
    std::vector<Diagnostic> diagnostics;
    bool has_errors() const;
};
ParamEnv resolve_parameters(const HirModule&, const std::map<std::string, Scalar>& overrides = {});
// 校验已有有效值及其约束，不重算默认式。独立参数偏导的扰动参考使用此接口。
std::vector<Diagnostic> validate_effective_parameters(const HirModule&,const ParamEnv&);

struct EvalInputs {
    std::map<std::string, double> node_voltages;
    std::optional<double> temperature_kelvin;
    std::size_t step_limit = 100000;
};
struct DiagnosticRecord {
    std::string function;
    std::vector<Scalar> arguments; // 不交给宿主 printf，避免把模型字符串当格式串执行
    Origin origin;
};
struct EvalResult {
    std::map<std::string, Scalar> observations; // 仅 module 作用域、实际已赋值的变量
    std::map<std::string, double> branch_flows; // 命名 branch 的带方向贡献和
    std::map<std::string, double> endpoint_flows; // 规范端点对，包括无名贡献
    std::vector<DiagnosticRecord> records;
    std::vector<Diagnostic> diagnostics;
    std::size_t steps = 0;
    bool has_errors() const;
};

// H3 直接执行 HIR 树；调用方提供偏置，不在此解 KCL/KVL 或调用 ngspice。
EvalResult evaluate_hir(const HirModule&, const ParamEnv&, const EvalInputs&);
void print_parameters(std::ostream&, const HirModule&, const ParamEnv& defaults, const ParamEnv& effective);
void print_evaluation(std::ostream&, const EvalResult&, std::string_view label="HIR H3");

} // namespace vagrad::hir
