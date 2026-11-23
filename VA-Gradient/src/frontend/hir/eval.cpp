#include "vagrad/frontend/hir/eval.hpp"
#include "vagrad/frontend/ast/ast.hpp"
#include "vagrad/runtime/scalar.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <ostream>
#include <set>

namespace vagrad::hir {
namespace {
bool errors(const std::vector<Diagnostic>& ds) {
    return std::any_of(ds.begin(), ds.end(), [](const auto& d) { return d.severity == Severity::Error; });
}
using runtime::Stop;
using FlowKey = std::pair<std::size_t, std::size_t>;
constexpr auto ground = std::numeric_limits<std::size_t>::max();
std::pair<FlowKey, double> flow_key(const Probe& p) {
    auto a = p.positive ? p.positive->value : ground;
    auto b = p.negative ? p.negative->value : ground;
    if (a > b) return {{b, a}, -1.0};
    return {{a, b}, 1.0};
}

// 此执行器只访问 HIR arena，不回读 AST 重新解释表达式或控制流。
class Machine : public runtime::ScalarOps {
public:
    Machine(const HirModule& module, std::vector<Diagnostic>& diagnostics)
        : ScalarOps(module,diagnostics),cells(module.bindings->symbols.size()) {}
    std::vector<std::optional<Scalar>> cells;
    std::function<Scalar(SymbolId, Origin)> read_parameter;
    std::vector<DiagnosticRecord> records;
    std::map<FlowKey, double> flows;

    Scalar expression(ExprId id) {
        const auto& e = m.expr(id);
        tick(e.origin);
        const auto eval = [&](std::size_t i) { return expression(e.operands.at(i)); };
        switch (e.kind) {
        case ExprKind::Literal: return e.literal;
        case ExprKind::Ref: {
            if (!e.symbol) fail(e.origin, "EVAL_HIR", "Ref without symbol");
            const auto kind = m.bindings->symbol(*e.symbol).kind;
            if (kind == SymbolKind::Parameter || kind == SymbolKind::LocalParameter)
                return read_parameter(*e.symbol, e.origin);
            if (!cells[e.symbol->value]) fail(e.origin, "EVAL_STATE",
                "variable read before assignment in this evaluation: " + m.bindings->symbol(*e.symbol).name +
                "; history-dependent execution is outside this profile");
            return *cells[e.symbol->value];
        }
        case ExprKind::Convert: return convert(eval(0), e.type, e.origin);
        case ExprKind::Temperature: return temperature(e.origin);
        case ExprKind::ReadPotential: {
            const auto a = voltage(e.probe->positive, e.origin);
            const auto b = voltage(e.probe->negative, e.origin);
            return finite(a - b, e.origin);
        }
        case ExprKind::Select: {
            const auto c = eval(0);
            return eval(number(c) != 0 ? 1 : 2); // 只求值选中分支
        }
        case ExprKind::Unary: return ScalarOps::unary(e.op,e.type,eval(0),e.origin);
        case ExprKind::Binary: return binary(e);
        case ExprKind::MathCall: return math(e);
        case ExprKind::DiagnosticCall: {
            if (!inputs) fail(e.origin, "EVAL_PARAMETER_CONSTANT", "diagnostic in parameter expression");
            DiagnosticRecord record{e.op, {}, e.origin};
            for (const auto arg : e.operands) record.arguments.push_back(expression(arg));
            records.push_back(std::move(record));
            return std::int32_t{0}; // Void 只允许作语句；H2 阻止其参与数值运算
        }
        default: fail(e.origin, "EVAL_HIR", "cannot execute Error expression");
        }
    }
    Scalar binary(const Expr& e) {
        const auto left=expression(e.operands[0]);
        if(e.op=="&&" && number(left)==0) return std::int32_t{0};
        if(e.op=="||" && number(left)!=0) return std::int32_t{1};
        const auto right=expression(e.operands[1]);
        return ScalarOps::binary(e.op,e.type,left,right,e.origin);
    }
    Scalar math(const Expr& e) {
        std::vector<Scalar> args;
        for(const auto id:e.operands) args.push_back(expression(id));
        return ScalarOps::math(e.op,args,e.origin);
    }
    void statement(StmtId id) {
        const auto& s = m.stmt(id); tick(s.origin);
        switch (s.kind) {
        case StmtKind::Empty: break;
        case StmtKind::Block: for (const auto child : s.children) statement(child); break;
        case StmtKind::If: {
            const auto c = expression(*s.expression);
            if (number(c) != 0) statement(s.children[0]);
            else if (s.children.size() > 1) statement(s.children[1]);
            break;
        }
        case StmtKind::Assign: {
            const auto value = expression(*s.expression);
            cells[s.target->value] = convert(value, m.symbol_types[s.target->value], s.origin);
            break;
        }
        case StmtKind::Contribute: {
            const auto value = number(expression(*s.expression));
            const auto [key, sign] = flow_key(*s.probe);
            if (key.first != key.second)
                flows[key] = number(finite(flows[key] + sign * value, s.origin));
            break;
        }
        case StmtKind::Expression: (void)expression(*s.expression); break;
        }
    }
};

Origin fallback_origin(const HirModule& m) {
    const ast::Node root(m.bindings->syntax, m.bindings->syntax.root());
    return {root.id(), root.first_token() ? root.first_token()->index() : 0};
}
} // namespace

bool ParamEnv::has_errors() const { return errors(diagnostics); }
bool EvalResult::has_errors() const { return errors(diagnostics); }

ParamEnv resolve_parameters(const HirModule& m, const std::map<std::string, Scalar>& overrides) {
    ParamEnv env;
    env.bindings = m.bindings;
    env.values.resize(m.bindings->symbols.size());
    if (m.has_errors()) {
        env.diagnostics.push_back(semantic_diagnostic(*m.bindings, fallback_origin(m),
            "EVAL_HIR_ERRORS", "parameter resolution requires valid typed HIR"));
        return env;
    }
    Machine machine(m, env.diagnostics);
    std::map<std::size_t, std::size_t> index;
    std::map<std::string, std::size_t> names;
    std::vector<unsigned> state(m.parameters.size());
    std::vector<std::optional<Scalar>> selected(m.parameters.size());
    for (std::size_t i = 0; i < m.parameters.size(); ++i) {
        const auto& p = m.parameters[i];
        index[p.symbol.value] = i;
        // 外部只能覆盖 module 参数，命名块内同名 localparam 不进入此表。
        if (m.bindings->symbol(p.symbol).scope == m.scope) names[m.bindings->symbol(p.symbol).name] = i;
    }
    try {
        for (const auto& [name, value] : overrides) {
            const auto it = names.find(name);
            if (it == names.end()) machine.fail(fallback_origin(m), "EVAL_PARAMETER", "unknown module parameter override: " + name);
            const auto& p = m.parameters[it->second];
            if (!p.overridable) machine.fail(p.origin, "EVAL_PARAMETER", "localparam cannot be overridden: " + name);
            selected[it->second] = machine.convert(value, p.type, p.origin);
            env.overridden.push_back(p.symbol);
        }
        std::vector<std::size_t> dependency_stack;
        std::function<Scalar(std::size_t, Origin)> resolve;
        machine.read_parameter = [&](SymbolId id, Origin where) -> Scalar {
            const auto it = index.find(id.value);
            if (it == index.end()) machine.fail(where, "EVAL_PARAMETER_CONSTANT", "non-parameter default dependency");
            return resolve(it->second, where);
        };
        resolve = [&](std::size_t i, Origin use) -> Scalar {
            const auto& p = m.parameters[i];
            if (state[i] == 2) return *env.values[p.symbol.value];
            if (dependency_stack.size() > 256) machine.fail(use, "EVAL_LIMIT", "parameter dependency depth limit exceeded");
            if (state[i] == 1) {
                auto d = semantic_diagnostic(*m.bindings, use, "EVAL_PARAMETER_CYCLE",
                    "cyclic effective default dependency at " + m.bindings->symbol(p.symbol).name);
                d.related.push_back({m.bindings->syntax.input().tokens[p.origin.token_index].original.span,
                                     "parameter declared here"});
                env.diagnostics.push_back(std::move(d)); throw Stop{};
            }
            state[i] = 1;
            dependency_stack.push_back(i);
            // 覆盖在前：若 A 被覆盖，则不执行 A 的默认式，即使默认式有循环或域问题。
            Scalar value;
            if (selected[i]) value = *selected[i];
            else {
                if (!p.default_value) machine.fail(p.origin, "EVAL_PARAMETER", "missing parameter default");
                value = machine.expression(*p.default_value);
            }
            env.values[p.symbol.value] = machine.convert(value, p.type, p.origin);
            state[i] = 2;
            dependency_stack.pop_back();
            return *env.values[p.symbol.value];
        };
        for (std::size_t i = 0; i < m.parameters.size(); ++i) (void)resolve(i, m.parameters[i].origin);
        const auto validation=validate_effective_parameters(m,env);
        env.diagnostics.insert(env.diagnostics.end(),validation.begin(),validation.end());
    } catch (const Stop&) {
        // 部分环境可用于诊断，但 has_errors() 阻止它被当作可执行环境。
    }
    return env;
}

std::vector<Diagnostic> validate_effective_parameters(const HirModule& m,const ParamEnv& env) {
    std::vector<Diagnostic> diagnostics;
    if(m.has_errors()||env.has_errors()||env.bindings!=m.bindings||env.values.size()!=m.bindings->symbols.size()) {
        diagnostics.push_back(semantic_diagnostic(*m.bindings,fallback_origin(m),"EVAL_PARAMETER",
            "validation requires a complete matching effective parameter environment"));
        return diagnostics;
    }
    Machine machine(m,diagnostics);
    machine.read_parameter = [&](SymbolId id,Origin use)->Scalar {
        if(!env.values[id.value]) machine.fail(use,"EVAL_PARAMETER","missing effective parameter value");
        return *env.values[id.value];
    };
    try {
        for(const auto& p:m.parameters) {
            if(!env.values[p.symbol.value]||scalar_type(*env.values[p.symbol.value])!=p.type)
                machine.fail(p.origin,"EVAL_PARAMETER","missing or wrongly typed effective parameter");
            (void)machine.finite(*env.values[p.symbol.value],p.origin);
        }
        const auto bound = [&](const Bound& b) {
            if (b.kind == Bound::Kind::NegativeInfinity) return -std::numeric_limits<double>::infinity();
            if (b.kind == Bound::Kind::PositiveInfinity) return std::numeric_limits<double>::infinity();
            return number(machine.expression(*b.expression));
        };
        for (const auto& p : m.parameters) {
            bool any_from = false, matches_from = false, excluded = false;
            if (p.constraints.empty()) continue;
            const auto value = number(*env.values[p.symbol.value]);
            for (const auto& c : p.constraints) {
                const auto low = bound(c.lower);
                const auto high = c.interval ? bound(c.upper) : low;
                if (low > high || (c.interval && low == high && (!c.lower_inclusive || !c.upper_inclusive)))
                    machine.fail(c.origin, "EVAL_PARAMETER_RANGE", "empty/reversed parameter constraint");
                const bool matches = c.interval ?
                    ((c.lower_inclusive ? value >= low : value > low) &&
                     (c.upper_inclusive ? value <= high : value < high)) : value == low;
                if (c.exclude) excluded = excluded || matches;
                else { any_from = true; matches_from = matches_from || matches; }
            }
            if ((any_from && !matches_from) || excluded)
                machine.fail(p.origin, "EVAL_PARAMETER_RANGE", "effective value violates constraints: " +
                    m.bindings->symbol(p.symbol).name + "=" + scalar_text(*env.values[p.symbol.value]));
        }
    } catch (const Stop&) {
        // 返回约束/类型错误，不改变调用方的环境。
    }
    return diagnostics;
}

EvalResult evaluate_hir(const HirModule& m, const ParamEnv& parameters, const EvalInputs& inputs) {
    EvalResult result;
    if (m.has_errors() || parameters.has_errors() || parameters.bindings != m.bindings ||
        parameters.values.size() != m.bindings->symbols.size()) {
        result.diagnostics.push_back(semantic_diagnostic(*m.bindings, fallback_origin(m),
            "EVAL_HIR_ERRORS", "evaluation requires valid HIR and a complete matching parameter environment"));
        return result;
    }
    Machine machine(m, result.diagnostics);
    machine.inputs = &inputs; machine.limit = inputs.step_limit;
    machine.read_parameter = [&](SymbolId id, Origin use) -> Scalar {
        if (!parameters.values[id.value]) machine.fail(use, "EVAL_PARAMETER", "missing effective parameter value");
        return machine.convert(*parameters.values[id.value], m.symbol_types[id.value], use);
    };
    try {
        std::set<std::string> net_names;
        for (const auto& s : m.bindings->symbols) if (s.kind == SymbolKind::Net) net_names.insert(s.name);
        for (const auto& [name, value] : inputs.node_voltages) {
            if (!net_names.contains(name)) machine.fail(fallback_origin(m), "EVAL_INPUT", "unknown node voltage input: " + name);
            if (!std::isfinite(value)) machine.fail(fallback_origin(m), "EVAL_INPUT", "node voltage must be finite: " + name);
        }
        if (inputs.temperature_kelvin) (void)machine.temperature(fallback_origin(m));
        if (!m.body) machine.fail(fallback_origin(m), "EVAL_HIR", "module has no execution body");
        // 模块变量在真实仿真中的初始化/保留不等于每次调用清零。这里用未赋值槽
        // 限定无历史的单点评估：每次读都必须由本次已执行的赋值支配。
        machine.statement(*m.body);
        for (const auto& v : m.variables) if (machine.cells[v.symbol.value] &&
            m.bindings->symbol(v.symbol).scope == m.scope)
            result.observations[m.bindings->symbol(v.symbol).name] = *machine.cells[v.symbol.value];
        for (const auto& b : m.branches) {
            const auto [key, sign] = flow_key(b.probe);
            const auto found = machine.flows.find(key);
            result.branch_flows[m.bindings->symbol(b.symbol).name] = found == machine.flows.end() ? 0.0 : sign * found->second;
        }
        const auto endpoint = [&](std::size_t id) { return id == ground ? std::string("0") : m.bindings->symbol({id}).name; };
        for (const auto& [key, value] : machine.flows)
            result.endpoint_flows[endpoint(key.first) + "," + endpoint(key.second)] = value;
    } catch (const Stop&) {
        // 不发布部分变量/贡献为成功数值；诊断和执行记录仍可查看。
    }
    result.steps = machine.steps;
    result.records = std::move(machine.records);
    return result;
}

void print_parameters(std::ostream& out, const HirModule& m, const ParamEnv& defaults, const ParamEnv& effective) {
    out << "Parameters (default and effective values are separate)\n";
    for (const auto& p : m.parameters) {
        out << "  " << m.bindings->symbol(p.symbol).name << ": " << type_name(p.type) << " default=";
        if (p.symbol.value < defaults.values.size() && defaults.values[p.symbol.value]) out << scalar_text(*defaults.values[p.symbol.value]);
        else out << "<unavailable>";
        out << " effective=";
        if (p.symbol.value < effective.values.size() && effective.values[p.symbol.value]) out << scalar_text(*effective.values[p.symbol.value]);
        else out << "<unavailable>";
        out << '\n';
    }
    out << "  default_status=" << (defaults.has_errors() ? "error" : "ok")
        << " effective_status=" << (effective.has_errors() ? "error" : "ok") << '\n';
}
void print_evaluation(std::ostream& out, const EvalResult& r, std::string_view label) {
    out << std::setprecision(17) << label << " status=" << (r.has_errors() ? "error" : "ok") << " steps=" << r.steps << '\n';
    for (const auto& [name, value] : r.observations) out << "  " << name << "@exit = " << scalar_text(value) << '\n';
    for (const auto& [name, value] : r.branch_flows) out << "  flow(" << name << ")@exit = " << value << '\n';
    for (const auto& [name, value] : r.endpoint_flows) out << "  flow(" << name << ")@exit = " << value << '\n';
    for (const auto& record : r.records) {
        out << "  diagnostic_record " << record.function;
        for (const auto& value : record.arguments) out << ' ' << scalar_text(value);
        out << " @token=" << record.origin.token_index << '\n';
    }
    for (const auto& d : r.diagnostics) out << "Diagnostic " << d.code << ": " << d.message << '\n';
}
} // namespace vagrad::hir
