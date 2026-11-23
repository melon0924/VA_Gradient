#pragma once
#include "vagrad/ad/forward.hpp"
#include "vagrad/ir/mir/mir.hpp"

namespace vagrad::ad {
// 一条 SSA 原值对应一个向量导数定义；局部 DAG 的节点编号只在该规则内有效。
enum class DerivativeKind { Zero, Undefined, Phi, Parameter, Expression, Discrete,
                            UnaryMath, BinaryMath, Power, Extremum };
enum class DerivativeOp { Zero, PrimalArgument, TangentArgument, PrimalResult,
                          Negate, Add, Subtract, Multiply, Divide };
struct DerivativeNode {
    DerivativeOp opcode;
    std::vector<std::size_t> inputs; // 子节点编号，必须小于本节点编号
    std::size_t argument=0;         // PrimalArgument/TangentArgument 的操作数位置
};
struct DerivativeRule {
    mir::ValueId result{0};
    mir::Opcode source=mir::Opcode::Constant;
    hir::Type type=hir::Type::Error;
    std::vector<mir::ValueId> arguments;
    std::string operation;
    DerivativeKind kind=DerivativeKind::Zero;
    std::vector<DerivativeNode> nodes;
    std::optional<std::size_t> root;
};
struct DerivativeIr {
    std::vector<DerivativeRule> rules; // 原值 ValueId 索引
    std::vector<Diagnostic> diagnostics;
    bool has_errors() const {return !diagnostics.empty();}
};
DerivativeIr lower_derivatives(const mir::MirFunction&);
std::vector<Diagnostic> verify_derivatives(const mir::MirFunction&,const DerivativeIr&);
// 参数由种子环境求值；phi 与 Undef 由 CFG 执行层处理。这里执行普通局部规则。
Jet evaluate_derivative(const DerivativeRule&,const std::vector<Jet>& arguments,double primal,std::size_t lanes);
void print_derivatives(std::ostream&,const mir::MirFunction&,const DerivativeIr&);
} // namespace vagrad::ad
