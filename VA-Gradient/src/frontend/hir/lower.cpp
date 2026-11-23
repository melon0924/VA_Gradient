#include "vagrad/frontend/hir/typed.hpp"
#include "vagrad/frontend/ast/ast.hpp"

#include <algorithm>
#include <functional>
#include <set>

namespace vagrad::hir {
namespace {
namespace A = vagrad::ast;
using K = SyntaxKind;

class Lowerer {
public:
    explicit Lowerer(std::shared_ptr<const BindingResult> bindings) : m_(std::move(bindings)) {}
    HirModule run() {
        const auto root = A::Root::from_tree(m_.bindings->syntax);
        if (m_.bindings->has_errors()) {
            error(root, "HIR_H1_ERRORS", "typed HIR requires successful H1 binding");
            return std::move(m_);
        }
        const auto modules = root.modules();
        if (modules.size() != 1) {
            error(root, "HIR_CAPABILITY", "static scalar profile requires exactly one module");
            return std::move(m_);
        }
        const auto module = modules.front();
        m_.name = module.name() ? canonical(*module.name()) : "";
        m_.scope = m_.bindings->scope_of(module.id());
        // module 节点本身的 scope 可能是其外部作用域，以定义的 inner_scope 为准。
        if (module.name()) {
            for (const auto id : m_.bindings->declared_at(module.name()->index()))
                if (m_.bindings->symbol(id).inner_scope) m_.scope = *m_.bindings->symbol(id).inner_scope;
        }
        m_.symbol_types.assign(m_.bindings->symbols.size(), Type::Error);
        for (std::size_t i = 0; i < m_.bindings->symbols.size(); ++i) {
            const auto& s = m_.bindings->symbols[i];
            if (s.kind == SymbolKind::Net) m_.symbol_types[i] = Type::Net;
            else if (s.kind == SymbolKind::Branch) m_.symbol_types[i] = Type::Branch;
            else if (s.kind == SymbolKind::Variable) m_.symbol_types[i] = declared_type(s.type_name);
        }
        collect(module);
        param_state_.resize(m_.parameters.size());
        for (std::size_t i = 0; i < m_.parameters.size(); ++i) ensure_parameter(i);
        for (const auto& branch : module.branches()) {
            const auto p = terminals(branch.terminals(), branch);
            for (const auto& name : branch.names()) if (const auto id = declared(name)) {
                auto value = p;
                value.alias = id;
                m_.branches.push_back({*id, value, origin(branch)});
            }
        }
        const auto blocks = module.analog_blocks();
        if (blocks.size() != 1) error(module, "HIR_CAPABILITY", "static scalar profile requires exactly one analog block");
        for (const auto& block : blocks) {
            if (block.is_initial()) error(block, "HIR_CAPABILITY", "analog initial requires simulation state");
            if (const auto body = block.body()) m_.body = statement(*body);
        }
        return std::move(m_);
    }
private:
    HirModule m_;
    std::vector<A::Declarator> param_decls_;
    std::vector<unsigned> param_state_;
    std::map<std::size_t, std::size_t> param_index_;
    std::size_t depth_ = 0;
    struct Depth {
        std::size_t& n;
        explicit Depth(std::size_t& d) : n(d) { ++n; }
        ~Depth() { --n; }
    };

    static std::string canonical(const A::Token& token) {
        auto text = token.text();
        if (token.kind() == TokenKind::EscapedIdentifier) text.remove_prefix(1);
        return std::string(text);
    }
    Origin origin(const A::Node& n) const {
        const auto token = n.first_token();
        return {n.id(), token ? token->index() : m_.bindings->syntax.input().tokens.size() - 1};
    }
    void error(const A::Node& n, std::string code, std::string message) {
        m_.diagnostics.push_back(semantic_diagnostic(*m_.bindings, origin(n), std::move(code), std::move(message)));
    }
    std::optional<SymbolId> declared(const A::Token& token) const {
        const auto ids = m_.bindings->declared_at(token.index());
        if (ids.empty()) return std::nullopt;
        return ids.front();
    }
    std::optional<SymbolId> reference(const A::Expr& expr) const {
        const auto name = A::cast<A::NameExpr>(expr.without_parens());
        if (!name || !name->name()) return std::nullopt;
        const auto id = m_.bindings->reference_at(name->name()->index());
        if (!id) return std::nullopt;
        const auto& r = m_.bindings->reference(*id);
        return r.status == ReferenceStatus::Resolved ? r.target : std::nullopt;
    }
    static Type declared_type(const std::optional<std::string>& name) {
        if (!name || *name == "real") return Type::Real;
        if (*name == "integer") return Type::Integer;
        if (*name == "string") return Type::String;
        return Type::Error;
    }
    ExprId add(Expr e) {
        const ExprId id{m_.expressions.size()};
        m_.expressions.push_back(std::move(e));
        return id;
    }
    ExprId bad_expr(const A::Node& node, std::string message, std::string code = "HIR_CAPABILITY") {
        error(node, std::move(code), std::move(message));
        Expr e; e.origin = origin(node);
        return add(std::move(e));
    }
    StmtId add(Stmt s) {
        const StmtId id{m_.statements.size()};
        m_.statements.push_back(std::move(s));
        return id;
    }
    Type type(ExprId e) const { return m_.expr(e).type; }
    bool raw_based_integer(ExprId id) const {
        const auto& e = m_.expr(id);
        return e.type == Type::Integer && e.format && e.format->spelling.find('\'') != std::string::npos;
    }
    ExprId convert(ExprId id, Type target, const A::Node& where) {
        if (type(id) == target || type(id) == Type::Error || target == Type::Error) return id;
        if (!numeric(type(id)) || !numeric(target))
            return bad_expr(where, "cannot convert " + std::string(type_name(type(id))) + " to " +
                            std::string(type_name(target)), "HIR_TYPE");
        Expr e; e.kind = ExprKind::Convert; e.type = target; e.origin = origin(where); e.operands = {id};
        return add(std::move(e));
    }
    void require_numeric(ExprId e, const A::Node& node) {
        if (!numeric(type(e)) && type(e) != Type::Error)
            error(node, "HIR_TYPE", "numeric operand required; got " + std::string(type_name(type(e))));
    }

    void collect(const A::Node& n) {
        if (n.kind() == K::AnalogFunctionDecl) {
            error(n, "HIR_CAPABILITY", "user analog functions are outside the first static scalar execution profile");
            return;
        }
        if (const auto d = A::cast<A::Declaration>(n)) {
            for (const auto& item : d->declarators()) {
                if (!item.name()) continue;
                const auto id = declared(*item.name());
                if (!id) continue;
                const auto& symbol = m_.bindings->symbol(*id);
                if (!item.array_ranges().empty()) error(item, "HIR_CAPABILITY", "array declarations are not supported");
                if (symbol.kind == SymbolKind::Parameter || symbol.kind == SymbolKind::LocalParameter) {
                    ParamInfo p;
                    p.symbol = *id; p.origin = origin(item);
                    p.overridable = symbol.kind == SymbolKind::Parameter;
                    // 无显式类型时稍后由默认表达式推导。
                    p.type = symbol.type_name ? declared_type(symbol.type_name) : Type::Error;
                    if (symbol.type_name && p.type == Type::Error)
                        error(item, "HIR_TYPE", "unsupported parameter type " + *symbol.type_name);
                    for (const auto& a : d->attributes()) if (a.name())
                        p.attributes[canonical(*a.name())] = {a.value() ? a.value()->text() : "", origin(a)};
                    m_.symbol_types[id->value] = p.type;
                    param_index_[id->value] = m_.parameters.size();
                    m_.parameters.push_back(std::move(p));
                    param_decls_.push_back(item);
                } else if (symbol.kind == SymbolKind::Variable) {
                    const auto t = m_.symbol_types[id->value];
                    m_.variables.push_back({*id, t, origin(item)});
                    if (t == Type::Error) error(item, "HIR_TYPE", "unsupported variable type");
                    if (item.initializer()) error(item, "HIR_CAPABILITY",
                        "declaration initializers require initialization/state semantics; assign in analog instead");
                } else if (symbol.kind == SymbolKind::AliasParameter)
                    error(item, "HIR_CAPABILITY", "aliasparam is not supported in typed HIR yet");
            }
        }
        for (const auto& child : n.children()) collect(child);
    }
    // 参数表达式只能依赖参数和纯运算，不可读取电位、温度或可写变量。
    void constant_expression(ExprId id, const A::Node& n) {
        const auto& e = m_.expr(id);
        if (e.kind == ExprKind::Ref && e.symbol && !param_index_.contains(e.symbol->value))
            error(n, "HIR_PARAMETER_CONSTANT", "parameter expression may only reference parameters");
        if (e.kind == ExprKind::Temperature || e.kind == ExprKind::ReadPotential ||
            e.kind == ExprKind::DiagnosticCall || (e.kind == ExprKind::MathCall && e.op == "$vt"))
            error(n, "HIR_PARAMETER_CONSTANT", "parameter expression must be independent of evaluation inputs/effects");
        for (const auto operand : e.operands) constant_expression(operand, n);
    }
    static bool atom_is(const A::Expr& expression, std::string_view spelling) {
        const auto n = expression.without_parens();
        // CST 的 text() 包含前导 trivia；语义判断始终读取有效 token。
        return (n.kind() == K::LiteralExpr || n.kind() == K::NameExpr) &&
            n.first_token() && n.first_token()->text() == spelling;
    }
    Bound bound(const std::optional<A::Expr>& input) {
        if (!input) return {};
        const auto e = input->without_parens();
        if (atom_is(e, "inf")) return {Bound::Kind::PositiveInfinity, {}};
        if (const auto u = A::cast<A::UnaryExpr>(e); u && u->op() && u->operand() &&
            (u->op()->text() == "+" || u->op()->text() == "-") && atom_is(*u->operand(), "inf"))
            return {u->op()->text() == "-" ? Bound::Kind::NegativeInfinity : Bound::Kind::PositiveInfinity, {}};
        const auto value = expression(e);
        require_numeric(value, e);
        constant_expression(value, e);
        return {Bound::Kind::Expression, value};
    }
    void ensure_parameter(std::size_t i) {
        if (param_state_[i] == 2) return;
        const auto declaration = param_decls_[i]; // arena 扩展不影响 AST 句柄
        if (param_state_[i] == 1) {
            error(declaration, "HIR_PARAMETER_TYPE", "cyclic inference of an untyped parameter; declare its type");
            return;
        }
        param_state_[i] = 1;
        const auto initializer = declaration.initializer();
        if (!initializer) error(declaration, "HIR_PARAMETER_DEFAULT", "parameter requires a default expression");
        else {
            auto value = expression(*initializer);
            if (!m_.bindings->symbol(m_.parameters[i].symbol).type_name && raw_based_integer(value))
                error(declaration, "HIR_CAPABILITY",
                    "untyped based parameter retains Verilog bit-width semantics; declare integer explicitly");
            if (m_.parameters[i].type == Type::Error && !m_.bindings->symbol(m_.parameters[i].symbol).type_name)
                m_.parameters[i].type = type(value);
            const auto t = m_.parameters[i].type;
            m_.symbol_types[m_.parameters[i].symbol.value] = t;
            if (!numeric(t) && t != Type::String && t != Type::Error)
                error(declaration, "HIR_TYPE", "parameter must have scalar numeric or string type");
            value = convert(value, t, declaration);
            m_.parameters[i].default_value = value;
            constant_expression(value, declaration);
        }
        for (const auto& r : declaration.parameter_ranges()) {
            Constraint c;
            c.origin = origin(r);
            c.exclude = r.keyword() && r.keyword()->text() == "exclude";
            c.interval = r.is_interval();
            c.lower_inclusive = r.lower_inclusive().value_or(true);
            c.upper_inclusive = r.upper_inclusive().value_or(true);
            if (m_.parameters[i].type == Type::String)
                error(r, "HIR_CAPABILITY", "string parameter constraints are not supported");
            c.lower = bound(c.interval ? r.lower() : r.single_value());
            if (c.interval) c.upper = bound(r.upper());
            m_.parameters[i].constraints.push_back(std::move(c));
        }
        param_state_[i] = 2;
    }
    Probe terminals(const std::vector<A::Expr>& args, const A::Node& node) {
        Probe p;
        if (args.empty() || args.size() > 2) {
            error(node, "HIR_SIGNATURE", "probe/branch requires one or two terminals");
            return p;
        }
        auto endpoint = [&](const A::Expr& e) -> std::optional<SymbolId> {
            if (const auto id = reference(e); id && m_.bindings->symbol(*id).kind == SymbolKind::Net) return id;
            // 仅支持参考地字面量 0；绝不把未知名称或任意数字当成地。
            if (atom_is(e, "0")) return std::nullopt;
            error(e, "HIR_PROBE", "terminal must be an electrical net or literal ground 0");
            return std::nullopt;
        };
        if (args.size() == 1) {
            if (const auto id = reference(args.front()); id && m_.bindings->symbol(*id).kind == SymbolKind::Branch) {
                const auto b = std::find_if(m_.branches.begin(), m_.branches.end(),
                    [&](const auto& item) { return item.symbol == *id; });
                if (b != m_.branches.end()) return b->probe;
                error(node, "HIR_PROBE", "branch alias has no terminal definition");
                return p;
            }
        }
        p.positive = endpoint(args[0]);
        if (args.size() == 2) p.negative = endpoint(args[1]);
        for (const auto id : {p.positive, p.negative}) if (id) {
            const auto& s = m_.bindings->symbol(*id);
            if (!s.type_name || *s.type_name != "electrical")
                error(node, "HIR_CAPABILITY", "static potential/flow evaluation currently requires electrical terminals");
        }
        return p;
    }
    ExprId binary(std::string op, ExprId left, ExprId right, const A::Node& n) {
        require_numeric(left, n); require_numeric(right, n);
        // 完整 Verilog 的位宽/无符号上下文会影响整数运算。首版只实现 int32：
        // based 常量可初始化 integer 或转 real，但不悄悄抹掉直接整数运算的位宽。
        if (type(left) == Type::Integer && type(right) == Type::Integer &&
            (raw_based_integer(left) || raw_based_integer(right)))
            return bad_expr(n, "based-literal integer arithmetic requires an explicitly typed integer parameter/variable first");
        const bool integer_only = op == "%" || op == "<<" || op == ">>" || op == "<<<" || op == ">>>" ||
            op == "&" || op == "|" || op == "^" || op == "~^" || op == "^~";
        if (integer_only && (type(left) != Type::Integer || type(right) != Type::Integer))
            error(n, "HIR_TYPE", "operator " + op + " requires Integer operands");
        const bool comparison = op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=";
        const bool logical = op == "&&" || op == "||";
        const std::set<std::string> supported{"+","-","*","/","**","%","<","<=",">",">=","==","!=",
            "&&","||","&","|","^","~^","^~","<<",">>","<<<",">>>"};
        if (!supported.contains(op)) return bad_expr(n, "unsupported binary operator " + op);
        Type result = (comparison || logical || integer_only) ? Type::Integer :
            (type(left) == Type::Real || type(right) == Type::Real ? Type::Real : Type::Integer);
        // 比较也使用共同数值类型；逻辑运算则保留操作数，解释器负责短路。
        if (!logical && !integer_only && (type(left) == Type::Real || type(right) == Type::Real)) {
            left = convert(left, Type::Real, n); right = convert(right, Type::Real, n);
        }
        Expr e; e.kind = ExprKind::Binary; e.type = result; e.origin = origin(n);
        e.op = std::move(op); e.operands = {left, right};
        return add(std::move(e));
    }
    ExprId expression(const A::Expr& input) {
        Depth guard(depth_);
        if (depth_ > 256) return bad_expr(input, "HIR expression nesting limit exceeded", "HIR_LIMIT");
        const auto n = input.without_parens();
        if (const auto literal = A::cast<A::LiteralExpr>(n); literal && literal->literal()) {
            Expr e; e.kind = ExprKind::Literal; e.origin = origin(n);
            const auto token = *literal->literal();
            if (token.kind() == TokenKind::StringLiteral) {
                std::string value;
                const auto text = token.text();
                for (std::size_t i = 1; i + 1 < text.size(); ++i) {
                    auto c = text[i];
                    if (c == '\\' && i + 2 < text.size()) {
                        c = text[++i];
                        if (c == 'n') c = '\n'; else if (c == 't') c = '\t';
                        else if (c != '\\' && c != '"')
                            return bad_expr(n, "unsupported string escape");
                    }
                    value += c;
                }
                e.type = Type::String; e.literal = std::move(value);
            } else {
                LiteralFormat format; std::string error_text;
                const auto value = decode_number(token.text(), format, error_text);
                if (!value) return bad_expr(n, error_text, "HIR_LITERAL");
                e.literal = *value; e.type = scalar_type(*value); e.format = std::move(format);
            }
            return add(std::move(e));
        }
        if (n.kind() == K::NameExpr) {
            const auto id = reference(n);
            if (!id) return bad_expr(n, "expression has no resolved H1 reference", "HIR_REFERENCE");
            const auto& s = m_.bindings->symbol(*id);
            Expr e; e.origin = origin(n); e.symbol = id;
            if (s.kind == SymbolKind::Environment) {
                if (s.name != "$temperature") return bad_expr(n, "time/state reads require simulation history");
                e.kind = ExprKind::Temperature; e.type = Type::Real; return add(std::move(e));
            }
            if (const auto p = param_index_.find(id->value); p != param_index_.end() &&
                m_.symbol_types[id->value] == Type::Error) ensure_parameter(p->second);
            e.kind = ExprKind::Ref; e.type = m_.symbol_types[id->value];
            if (e.type == Type::Error) return bad_expr(n, "name has no supported scalar type", "HIR_TYPE");
            return add(std::move(e));
        }
        if (const auto u = A::cast<A::UnaryExpr>(n); u && u->op() && u->operand()) {
            const std::string op(u->op()->text());
            if (op != "+" && op != "-" && op != "!" && op != "~") return bad_expr(n, "unsupported unary operator " + op);
            const auto value = expression(*u->operand());
            require_numeric(value, n);
            if (op != "!" && raw_based_integer(value))
                return bad_expr(n, "unary operation on a raw based literal needs full bit-width semantics");
            if (op == "~" && type(value) != Type::Integer) error(n, "HIR_TYPE", "~ requires Integer");
            Expr e; e.kind = ExprKind::Unary; e.type = op == "!" ? Type::Integer : type(value);
            e.origin = origin(n); e.op = op; e.operands = {value};
            return add(std::move(e));
        }
        if (const auto b = A::cast<A::BinaryExpr>(n); b && b->lhs() && b->rhs() && b->op()) {
            const auto left = expression(*b->lhs());
            const auto right = expression(*b->rhs());
            return binary(std::string(b->op()->text()), left, right, n);
        }
        if (const auto c = A::cast<A::ConditionalExpr>(n); c && c->condition() && c->true_value() && c->false_value()) {
            const auto condition = expression(*c->condition()); require_numeric(condition, n);
            auto yes = expression(*c->true_value()); auto no = expression(*c->false_value());
            auto t = type(yes);
            if (t == Type::Integer && type(no) == Type::Integer && (raw_based_integer(yes) || raw_based_integer(no)))
                return bad_expr(n, "selection of raw based Integer literals needs full bit-width semantics");
            if (numeric(t) && numeric(type(no))) {
                if (type(no) == Type::Real) t = Type::Real;
                yes = convert(yes, t, n); no = convert(no, t, n);
            } else if (t != Type::String || type(no) != Type::String)
                return bad_expr(n, "conditional branches require compatible scalar types", "HIR_TYPE");
            Expr e; e.kind = ExprKind::Select; e.type = t; e.origin = origin(n); e.operands = {condition, yes, no};
            return add(std::move(e));
        }
        if (const auto call = A::cast<A::CallExpr>(n)) return call_expression(*call);
        return bad_expr(n, "unsupported expression " + std::string(syntax_kind_name(n.kind())));
    }
    ExprId call_expression(const A::CallExpr& call) {
        const auto callee = call.callee();
        const auto id = callee ? reference(*callee) : std::nullopt;
        if (!id) return bad_expr(call, "callee has no resolved H1 symbol", "HIR_REFERENCE");
        const auto& s = m_.bindings->symbol(*id);
        if (s.builtin == BuiltinKind::None) return bad_expr(call, "user-defined calls/access functions are not supported");
        const auto args = call.arguments();
        if (s.builtin == BuiltinKind::Access) {
            if (s.name != "V") return bad_expr(call, "current probe requires a circuit solution; only potential reads are supported");
            Expr e; e.kind = ExprKind::ReadPotential; e.type = Type::Real; e.origin = origin(call);
            e.probe = terminals(args, call);
            return add(std::move(e));
        }
        auto name = s.name;
        if (s.builtin == BuiltinKind::Math && name.starts_with('$')) name.erase(0, 1);
        const auto& registry = builtin_registry();
        const auto sig = std::find_if(registry.begin(), registry.end(), [&](const auto& v) { return v.name == name; });
        if (sig == registry.end()) return bad_expr(call, "builtin has no executable signature: " + name);
        if (sig->effect == Effect::Stateful) return bad_expr(call, name + " requires unsupported state/analysis semantics");
        if (args.size() < sig->min_args || args.size() > sig->max_args)
            return bad_expr(call, "argument count mismatch for " + name, "HIR_SIGNATURE");
        Expr e; e.kind = sig->effect == Effect::Diagnostic ? ExprKind::DiagnosticCall : ExprKind::MathCall;
        e.type = sig->result;
        e.origin = origin(call); e.op = name;
        for (std::size_t i = 0; i < args.size(); ++i) {
            auto value = expression(args[i]);
            const auto rule = i < sig->arguments.size() ? sig->arguments[i] : *sig->variadic;
            if (rule == ArgumentType::String) {
                if (type(value) != Type::String) error(args[i], "HIR_SIGNATURE", "argument must be String");
            } else if (rule == ArgumentType::Numeric) {
                require_numeric(value, args[i]); value = convert(value, Type::Real, args[i]);
            } else if (type(value) != Type::String) require_numeric(value, args[i]);
            e.operands.push_back(value);
        }
        return add(std::move(e));
    }
    StmtId statement(const A::Statement& node) {
        Depth guard(depth_);
        Stmt s; s.origin = origin(node);
        if (depth_ > 256) { error(node, "HIR_LIMIT", "HIR statement nesting limit exceeded"); return add(std::move(s)); }
        if (const auto block = A::cast<A::BlockStmt>(node)) {
            s.kind = StmtKind::Block;
            for (const auto& child : block->statements()) s.children.push_back(statement(child));
        } else if (const auto branch = A::cast<A::IfStmt>(node); branch && branch->condition() && branch->then_branch()) {
            s.kind = StmtKind::If; s.expression = expression(*branch->condition());
            require_numeric(*s.expression, node);
            s.children.push_back(statement(*branch->then_branch()));
            if (branch->else_branch()) s.children.push_back(statement(*branch->else_branch()));
        } else if (const auto a = A::cast<A::Assignment>(node); a && a->lhs() && a->rhs() && a->op()) {
            auto value = expression(*a->rhs());
            if (node.kind() == K::ContributionStmt) {
                s.kind = StmtKind::Contribute;
                const auto call = A::cast<A::CallExpr>(a->lhs()->without_parens());
                const auto id = call && call->callee() ? reference(*call->callee()) : std::nullopt;
                if (!id || m_.bindings->symbol(*id).name != "I" || m_.bindings->symbol(*id).builtin != BuiltinKind::Access)
                    error(node, "HIR_CAPABILITY", "static scalar profile supports only electrical flow contributions I(...) <+ ...");
                else s.probe = terminals(call->arguments(), node);
                require_numeric(value, node);
                s.expression = convert(value, Type::Real, node);
            } else {
                s.kind = StmtKind::Assign;
                const auto id = reference(*a->lhs());
                if (!id || m_.bindings->symbol(*id).kind != SymbolKind::Variable)
                    error(*a->lhs(), "HIR_LVALUE", "assignment target must be a writable scalar variable");
                else {
                    s.target = id;
                    const auto op = a->op()->text();
                    if (op != "=") {
                        if (op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=")
                            value = binary(std::string(op.substr(0, 1)), expression(*a->lhs()), value, node);
                        else error(node, "HIR_CAPABILITY", "unsupported assignment operator " + std::string(op));
                    }
                    value = convert(value, m_.symbol_types[id->value], node);
                }
                s.expression = value;
            }
        } else if (const auto e = A::cast<A::ExprStmt>(node); e && e->expression()) {
            s.kind = StmtKind::Expression; s.expression = expression(*e->expression());
            const auto t=type(*s.expression);
            if(t==Type::Net || t==Type::Branch)
                error(node,"HIR_TYPE","expression statement requires a scalar value or diagnostic call");
        } else if (node.kind() != K::EmptyStmt)
            error(node, "HIR_CAPABILITY", "unsupported static statement " + std::string(syntax_kind_name(node.kind())));
        return add(std::move(s));
    }
};
} // namespace
HirModule lower_hir(std::shared_ptr<const BindingResult> bindings) { return Lowerer(std::move(bindings)).run(); }
} // namespace vagrad::hir
