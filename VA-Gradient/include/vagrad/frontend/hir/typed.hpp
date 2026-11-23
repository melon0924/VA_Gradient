#pragma once

#include "vagrad/frontend/hir/hir.hpp"

#include <cstdint>
#include <memory>
#include <variant>

namespace vagrad::hir {

// H2 只描述结构化语义，不在这里创建 MIR Value、基本块或 SSA phi。
enum class Type { Real, Integer, String, Net, Branch, Void, Error };
using Scalar = std::variant<std::int32_t, double, std::string>;
struct ExprId { std::size_t value; friend bool operator==(ExprId, ExprId) = default; };
struct StmtId { std::size_t value; friend bool operator==(StmtId, StmtId) = default; };
enum class ExprKind { Error, Literal, Ref, Convert, Unary, Binary, Select,
                      ReadPotential, Temperature, MathCall, DiagnosticCall };
enum class StmtKind { Empty, Block, If, Assign, Contribute, Expression };
enum class Effect { PureMath, Environment, Diagnostic, Stateful };
enum class ArgumentType { Numeric, String, Scalar };
struct BuiltinSignature {
    std::string_view name;
    Effect effect;
    std::size_t min_args, max_args;
    std::vector<ArgumentType> arguments;
    std::optional<ArgumentType> variadic;
    Type result;
};
const std::vector<BuiltinSignature>& builtin_registry();

// null 端点代表全局参考地；保留次序，V(a,b) 与 V(b,a) 的符号相反。
struct Probe {
    std::optional<SymbolId> positive, negative, alias;
};
struct LiteralFormat {
    std::string spelling;
    unsigned width = 32, radix = 10;
    bool explicitly_signed = false;
};
struct Expr {
    ExprKind kind = ExprKind::Error;
    Type type = Type::Error;
    Origin origin;
    std::vector<ExprId> operands;
    std::string op;
    std::optional<SymbolId> symbol;
    Scalar literal = std::int32_t{0};
    std::optional<Probe> probe;
    std::optional<LiteralFormat> format;
};
struct Stmt {
    StmtKind kind = StmtKind::Empty;
    Origin origin;
    std::vector<StmtId> children; // Block: 源码顺序；If: then / 可选 else
    std::optional<ExprId> expression;
    std::optional<SymbolId> target; // Assign 写入的源码变量
    std::optional<Probe> probe;    // Contribute: 静态 flow 贡献
};
struct Bound {
    enum class Kind { Expression, NegativeInfinity, PositiveInfinity };
    Kind kind = Kind::Expression;
    std::optional<ExprId> expression;
};
struct Constraint {
    bool exclude = false, interval = false;
    bool lower_inclusive = false, upper_inclusive = false;
    Bound lower, upper;
    Origin origin;
};
struct AttributeInfo { std::string text; Origin origin; };
struct ParamInfo {
    SymbolId symbol;
    Type type = Type::Error;
    bool overridable = true;
    std::optional<ExprId> default_value;
    std::vector<Constraint> constraints;
    // 保存属性原文及来源，不把 units="A/V^2" 误当成量纲类型推导。
    std::map<std::string, AttributeInfo> attributes;
    Origin origin;
};
struct VariableInfo { SymbolId symbol; Type type; Origin origin; };
struct BranchInfo { SymbolId symbol; Probe probe; Origin origin; };
struct HirModule {
    explicit HirModule(std::shared_ptr<const BindingResult> source) : bindings(std::move(source)) {}
    std::shared_ptr<const BindingResult> bindings;
    std::string name;
    ScopeId scope;
    std::vector<Type> symbol_types; // 以 H1 SymbolId 索引
    std::vector<ParamInfo> parameters;
    std::vector<VariableInfo> variables;
    std::vector<BranchInfo> branches;
    std::vector<Expr> expressions;
    std::vector<Stmt> statements;
    std::optional<StmtId> body;
    std::vector<Diagnostic> diagnostics;
    bool has_errors() const;
    const Expr& expr(ExprId id) const { return expressions.at(id.value); }
    const Stmt& stmt(StmtId id) const { return statements.at(id.value); }
};

// 当前静态标量 profile：一个 module，一个普通 analog 块；超出能力须诊断。
HirModule lower_hir(std::shared_ptr<const BindingResult> bindings);
Diagnostic semantic_diagnostic(const BindingResult&, Origin, std::string code, std::string message);
bool numeric(Type type);
Type scalar_type(const Scalar& value);
double number(const Scalar& value);
std::string scalar_text(const Scalar& value);
std::string_view type_name(Type type);
std::string_view expr_kind_name(ExprKind kind);
std::string_view stmt_kind_name(StmtKind kind);

// CLI 与 HIR 共用严格字面量解码；失败返回原因，不接受尾部垃圾或 x/z。
std::optional<Scalar> decode_number(std::string_view text, LiteralFormat& format, std::string& error);
void print_typed_hir(std::ostream&, const HirModule&);

} // namespace vagrad::hir
