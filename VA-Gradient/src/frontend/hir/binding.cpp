#include "vagrad/frontend/hir/hir.hpp"
#include "vagrad/frontend/ast/ast.hpp"

#include <algorithm>
#include <initializer_list>
#include <set>
#include <utility>

namespace vagrad::hir {
namespace {
namespace A = vagrad::ast;

std::string canonical(const A::Token& token) {
    auto text = token.text();
    if (token.kind() == TokenKind::EscapedIdentifier && !text.empty()) text.remove_prefix(1);
    return std::string(text);
}
bool primitive(const A::Token& token) {
    return token.kind() == TokenKind::Keyword;
}
bool net_type(const A::Token& token) {
    return !primitive(token) || token.text() == "wire" || token.text() == "wreal";
}
bool callable(SymbolKind kind) {
    return kind == SymbolKind::Function || kind == SymbolKind::AccessFunction ||
           kind == SymbolKind::BuiltinFunction;
}
std::optional<PortDirection> direction(const std::optional<A::Token>& token) {
    if (!token) return std::nullopt;
    if (token->text() == "input") return PortDirection::Input;
    if (token->text() == "output") return PortDirection::Output;
    if (token->text() == "inout") return PortDirection::Inout;
    return std::nullopt;
}

class Binder {
public:
    explicit Binder(const SyntaxTree& syntax) : result_(syntax) {}

    BindingResult run() {
        prelude_ = add_scope(ScopeKind::Prelude, "<prelude>", std::nullopt, std::nullopt, std::nullopt);
        root_ = add_scope(ScopeKind::CompilationUnit, "<root>", prelude_, result_.syntax.root(), std::nullopt);
        result_.node_scopes.assign(result_.syntax.node_count(), root_);
        parents_.resize(result_.syntax.node_count());
        seed_prelude();
        const auto root = A::Root::from_tree(result_.syntax);
        if (root.contains_error_node()) {
            diagnose(origin(root), "HIR_SYNTAX", "name binding requires a syntax tree without ErrorNode");
            return std::move(result_);
        }
        collect(root);       // 第一遍：定义与作用域
        classify();          // 标记读写、调用、类型和特殊语法角色
        resolve();           // 第二遍：按词法作用域将引用关联到定义
        return std::move(result_);
    }

private:
    struct Definition { SymbolId id; bool created; };
    struct Pending {
        Origin origin;
        ScopeId scope;
        NameSpace name_space;
        ReferenceRole role;
        bool forced = false;
        std::optional<SymbolId> target;
    };
    struct Work { A::Node node; ScopeId scope; std::optional<NodeId> parent; };
    struct PortState {
        std::optional<PortDirection> direction;
        std::optional<A::Token> type;
    };

    Origin origin(const A::Node& node, const A::Token& token) const { return {node.id(), token.index()}; }
    Origin origin(const A::Node& node) const {
        const auto token = node.first_token();
        return {node.id(), token ? token->index() : result_.syntax.input().tokens.size() - 1};
    }
    A::Token token(const Origin& source) const { return A::Token(result_.syntax, source.token_index); }
    A::Node node(NodeId id) const { return A::Node(result_.syntax, id); }

    void diagnose(Origin source, std::string code, std::string message,
                  std::optional<Origin> previous = std::nullopt) {
        const auto item = token(source);
        Diagnostic diagnostic{Severity::Error, std::move(code), std::move(message), item.source_span(), {}};
        if (previous) diagnostic.related.push_back({token(*previous).source_span(), "previous declaration is here"});
        auto current = std::optional<std::size_t>{item.context_index()};
        while (current) {
            const auto& context = result_.syntax.input().contexts.at(*current);
            if (context.call_site) {
                diagnostic.related.push_back({*context.call_site,
                    context.kind == ExpansionKind::Include ? "included from here" : "expanded from here"});
            }
            if (context.definition) diagnostic.related.push_back({*context.definition, "macro definition/formal"});
            current = context.parent;
        }
        result_.diagnostics.push_back(std::move(diagnostic));
    }
    ScopeId add_scope(ScopeKind kind, std::string name, std::optional<ScopeId> parent,
                      std::optional<NodeId> owner, std::optional<SymbolId> symbol) {
        const ScopeId id{result_.scopes.size()};
        result_.scopes.push_back({kind, std::move(name), parent, owner, symbol, {}, {}, {}});
        if (parent) result_.scopes[parent->value].children.push_back(id);
        return id;
    }
    std::map<std::string, SymbolId>& names(ScopeId scope, NameSpace space) {
        auto& data = result_.scopes.at(scope.value);
        return space == NameSpace::Value ? data.values : data.types;
    }
    std::optional<SymbolId> local(ScopeId scope, NameSpace space, const std::string& name) {
        const auto& table = names(scope, space);
        const auto found = table.find(name);
        return found == table.end() ? std::nullopt : std::optional<SymbolId>{found->second};
    }
    std::optional<Origin> first_site(SymbolId id) const {
        const auto& sites = result_.symbol(id).declarations;
        return sites.empty() ? std::nullopt : std::optional<Origin>{sites[0].origin};
    }
    std::optional<Origin> facet_site(SymbolId id, DeclarationRole role) const {
        for (const auto& site : result_.symbol(id).declarations) {
            if (site.role == role) return site.origin;
        }
        return std::nullopt;
    }
    SymbolId append_symbol(ScopeId scope, NameSpace space, std::string name, SymbolKind kind,
                           BuiltinKind builtin = BuiltinKind::None) {
        const SymbolId id{result_.symbols.size()};
        Symbol data;
        data.name = std::move(name);
        data.kind = kind;
        data.name_space = space;
        data.scope = scope;
        data.builtin = builtin;
        result_.symbols.push_back(std::move(data));
        names(scope, space).emplace(result_.symbol(id).name, id);
        return id;
    }
    Definition define(ScopeId scope, NameSpace space, const A::Token& name, SymbolKind kind,
                      Origin source, DeclarationRole role = DeclarationRole::Definition) {
        const auto key = canonical(name);
        if (const auto prior = local(scope, space, key)) {
            diagnose(source, "HIR_DUPLICATE", "duplicate definition of '" + key + "'", first_site(*prior));
            return {*prior, false};
        }
        if (name.kind() == TokenKind::SystemIdentifier) {
            diagnose(source, "HIR_DECL_NAME", "system names cannot be declared as user symbols");
        }
        const auto id = append_symbol(scope, space, key, kind);
        result_.symbols[id.value].declarations.push_back({role, source, {}});
        return {id, true};
    }
    void builtin(std::string name, SymbolKind kind, NameSpace space, BuiltinKind group) {
        (void)append_symbol(prelude_, space, std::move(name), kind, group);
    }
    void seed_prelude() {
        builtin("electrical", SymbolKind::Discipline, NameSpace::Type, BuiltinKind::Type);
        builtin("Voltage", SymbolKind::Nature, NameSpace::Type, BuiltinKind::Type);
        builtin("Current", SymbolKind::Nature, NameSpace::Type, BuiltinKind::Type);
        for (const auto name : {"V", "I"}) {
            builtin(name, SymbolKind::AccessFunction, NameSpace::Value, BuiltinKind::Access);
        }
        for (const auto name : {"min", "max", "abs", "sqrt", "exp", "ln", "log", "sin", "cos",
                               "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh",
                               "asinh", "acosh", "atanh", "ceil", "floor"}) {
            builtin(name, SymbolKind::BuiltinFunction, NameSpace::Value, BuiltinKind::Math);
            builtin("$" + std::string(name), SymbolKind::BuiltinFunction, NameSpace::Value, BuiltinKind::Math);
        }
        for (const auto name : {"$temperature", "$abstime"}) {
            builtin(name, SymbolKind::Environment, NameSpace::Value, BuiltinKind::Environment);
        }
        for (const auto name : {"$vt", "$simparam", "$param_given", "$port_connected"}) {
            builtin(name, SymbolKind::BuiltinFunction, NameSpace::Value, BuiltinKind::Environment);
        }
        for (const auto name : {"$strobe", "$display", "$write"}) {
            builtin(name, SymbolKind::BuiltinFunction, NameSpace::Value, BuiltinKind::Diagnostic);
        }
        for (const auto name : {"limexp", "ddt", "idt", "transition", "slew", "white_noise", "flicker_noise"}) {
            builtin(name, SymbolKind::BuiltinFunction, NameSpace::Value, BuiltinKind::AnalogOperator);
        }
        for (const auto name : {"continuous", "discrete"}) {
            builtin(name, SymbolKind::Domain, NameSpace::Value, BuiltinKind::Domain);
        }
    }

    std::optional<ScopeId> enclosing(ScopeId from, ScopeKind kind) const {
        auto current = std::optional<ScopeId>{from};
        while (current) {
            if (result_.scope(*current).kind == kind) return current;
            current = result_.scope(*current).parent;
        }
        return std::nullopt;
    }
    bool inside(ScopeId from, ScopeId ancestor) const {
        auto current = std::optional<ScopeId>{from};
        while (current) {
            if (*current == ancestor) return true;
            current = result_.scope(*current).parent;
        }
        return false;
    }
    std::optional<SymbolId> lookup(ScopeId from, NameSpace space, const std::string& name) const {
        const auto module = enclosing(from, ScopeKind::Module);
        auto current = std::optional<ScopeId>{from};
        while (current) {
            const auto& scope = result_.scope(*current);
            const auto& table = space == NameSpace::Value ? scope.values : scope.types;
            const auto found = table.find(name);
            if (found != table.end()) {
                const auto& symbol = result_.symbol(found->second);
                // 自定义 access 名只有在定义之后的模块中可见；prelude 始终可见。
                bool visible = true;
                if (module && symbol.kind == SymbolKind::AccessFunction &&
                    symbol.builtin == BuiltinKind::None && !symbol.declarations.empty()) {
                    const auto owner = result_.scope(*module).owner_node.value();
                    visible = symbol.declarations[0].origin.token_index <
                              result_.syntax.node(owner).token_begin;
                }
                if (visible) return found->second;
            }
            current = scope.parent;
        }
        return std::nullopt;
    }
    std::optional<SymbolId> lookup_call(ScopeId from, const std::string& name) const {
        auto target = lookup(from, NameSpace::Value, name);
        if (target && result_.symbol(*target).kind == SymbolKind::FunctionResult) {
            // 函数体中的 fn=... 绑定返回变量；fn(...) 绑定函数本身。
            if (result_.symbol(*target).return_of) target = result_.symbol(*target).return_of;
        }
        return target;
    }
    void type_reference(const A::Node& owner, ScopeId scope, const std::optional<A::Token>& type) {
        if (type && !primitive(*type)) {
            pending_.push_back({origin(owner, *type), scope, NameSpace::Type, ReferenceRole::DisciplineType});
        }
    }
    SymbolId new_port(ScopeId scope, const A::Token& name, Origin source, bool header) {
        const auto key = canonical(name);
        if (const auto prior = local(scope, NameSpace::Value, key)) {
            if (header || !result_.symbol(*prior).is_port) {
                diagnose(source, "HIR_DUPLICATE", "conflicting port definition of '" + key + "'", first_site(*prior));
            }
            return *prior;
        }
        const bool function = result_.scope(scope).kind == ScopeKind::Function;
        const auto id = append_symbol(scope, NameSpace::Value, key,
                                      function ? SymbolKind::Variable : SymbolKind::Net);
        auto& symbol = result_.symbols[id.value];
        symbol.is_port = true;
        if (header) symbol.declarations.push_back({DeclarationRole::PortHeader, source, {}});
        if (name.kind() == TokenKind::SystemIdentifier) {
            diagnose(source, "HIR_DECL_NAME", "system names cannot be declared as ports");
        }
        return id;
    }
    void merge_direction(SymbolId id, PortDirection value, Origin source) {
        auto& symbol = result_.symbols[id.value];
        if (!symbol.is_port) return;
        if (symbol.direction) {
            const bool different = *symbol.direction != value;
            diagnose(source, different ? "HIR_PORT_CONFLICT" : "HIR_REDECLARATION",
                     different ? "conflicting port direction for '" + symbol.name + "'" :
                                 "repeated port direction for '" + symbol.name + "'",
                     facet_site(id, DeclarationRole::Direction));
            return;
        }
        symbol.direction = value;
        symbol.declarations.push_back({DeclarationRole::Direction, source, std::string(port_direction_name(value))});
    }
    void merge_type(SymbolId id, const A::Token& type, Origin source) {
        auto& symbol = result_.symbols[id.value];
        const auto name = canonical(type);
        if (symbol.type_name) {
            const bool different = *symbol.type_name != name;
            diagnose(source, different ? "HIR_PORT_CONFLICT" : "HIR_REDECLARATION",
                     different ? "conflicting type declaration for '" + symbol.name + "'" :
                                 "repeated type declaration for '" + symbol.name + "'",
                     facet_site(id, DeclarationRole::DataType).has_value() ?
                         facet_site(id, DeclarationRole::DataType) : first_site(id));
            return;
        }
        symbol.type_name = name;
        symbol.kind = net_type(type) ? SymbolKind::Net : SymbolKind::Variable;
        symbol.declarations.push_back({DeclarationRole::DataType, source, name});
    }

    void collect_port(const A::Port& port, ScopeId scope) {
        const auto name = port.name();
        if (!name) {
            diagnose(origin(port), "HIR_MISSING_NAME", "port requires a name");
            return;
        }
        const auto id = new_port(scope, *name, origin(port, *name), true);
        const auto explicit_direction = direction(port.direction());
        const auto explicit_type = port.data_type();
        // ANSI 列表 input real a,b 中，b 继承前项方向和类型；
        // 遇到新的方向声明时，未写出的类型不继承上一组。
        PortState state;
        if (const auto parent = parents_[port.id().value]) {
            auto& inherited = port_states_[parent->value];
            if (explicit_direction) inherited = {*explicit_direction, explicit_type};
            else if (explicit_type) inherited.type = explicit_type;
            state = inherited;
        }
        if (state.direction) merge_direction(id, *state.direction, origin(port, *name));
        if (state.type) merge_type(id, *state.type, origin(port, *name));
        type_reference(port, scope, port.data_type());
    }
    void collect_declaration(const A::Declaration& declaration, ScopeId scope) {
        const auto kind = declaration.kind();
        const auto type = declaration.data_type();
        const auto scope_kind = result_.scope(scope).kind;
        if (const auto parent = parents_[declaration.id().value]) {
            if (const auto block = A::cast<A::BlockStmt>(node(*parent)); block && !block->label()) {
                diagnose(origin(declaration), "HIR_DECL_SCOPE", "declarations in an unnamed block are not supported");
            }
        }
        const bool net = kind == SyntaxKind::NetDecl || (type && net_type(*type));
        if (net && scope_kind != ScopeKind::Module) {
            diagnose(origin(declaration), "HIR_DECL_SCOPE", "nets must be declared in module scope");
        }
        if (kind == SyntaxKind::PortDecl && scope_kind != ScopeKind::Module && scope_kind != ScopeKind::Function) {
            diagnose(origin(declaration), "HIR_DECL_SCOPE", "port directions require module or function scope");
        }
        type_reference(declaration, scope, type);
        for (const auto& item : declaration.declarators()) {
            const auto name = item.name();
            if (!name) {
                diagnose(origin(item), "HIR_MISSING_NAME", "declarator requires a name");
                continue;
            }
            const auto source = origin(item, *name);
            const auto key = canonical(*name);
            if (kind == SyntaxKind::PortDecl) {
                const auto prior = local(scope, NameSpace::Value, key);
                if (scope_kind == ScopeKind::Module &&
                    (!prior || !facet_site(*prior, DeclarationRole::PortHeader))) {
                    diagnose(source, "HIR_PORT_HEADER", "port '" + key + "' is not declared in the module header");
                }
                const auto id = prior && result_.symbol(*prior).is_port ?
                    *prior : new_port(scope, *name, source, false);
                if (const auto value = direction(declaration.keyword())) merge_direction(id, *value, source);
                if (type) merge_type(id, *type, source);
            } else if (kind != SyntaxKind::ParameterDecl) {
                const auto prior = local(scope, NameSpace::Value, key);
                if (prior && result_.symbol(*prior).is_port) {
                    if (type) merge_type(*prior, *type, source);
                } else {
                    const auto symbol_kind = net ? SymbolKind::Net : SymbolKind::Variable;
                    const auto definition = define(scope, NameSpace::Value, *name, symbol_kind, source);
                    if (definition.created && type) result_.symbols[definition.id.value].type_name = canonical(*type);
                }
            } else {
                const auto keyword = declaration.keyword();
                const auto symbol_kind = keyword && keyword->text() == "localparam" ? SymbolKind::LocalParameter :
                    keyword && keyword->text() == "aliasparam" ? SymbolKind::AliasParameter : SymbolKind::Parameter;
                const auto definition = define(scope, NameSpace::Value, *name, symbol_kind, source);
                if (definition.created && type) result_.symbols[definition.id.value].type_name = canonical(*type);
            }
        }
    }
    void close_label(const A::Node& owner, ScopeId scope, std::string_view ending,
                     std::optional<SymbolId> target) {
        bool ended = false;
        bool colon = false;
        for (const auto& item : owner.tokens()) {
            if (item.text() == ending) {
                ended = true;
            } else if (ended && item.text() == ":") {
                colon = true;
            } else if (ended && colon &&
                       (item.kind() == TokenKind::Identifier || item.kind() == TokenKind::EscapedIdentifier)) {
                pending_.push_back({origin(owner, item), scope,
                    ending == "endmodule" ? NameSpace::Type : NameSpace::Value,
                    ReferenceRole::EndLabel, true, target});
                return;
            }
        }
    }
    void collect_property(const A::PropertyDecl& property, ScopeId scope) {
        const auto key = property.name();
        const auto value = property.value();
        if (!key || !value) return;
        const auto text = key->text();
        const auto kind = result_.scope(scope).kind;
        if (kind == ScopeKind::Nature && text == "access") {
            const auto name_expr = A::cast<A::NameExpr>(value->without_parens());
            if (!name_expr || !name_expr->name()) {
                diagnose(origin(property), "HIR_ACCESS_NAME", "access property requires an identifier");
                return;
            }
            const auto name = name_expr->name().value();
            (void)define(root_, NameSpace::Value, name, SymbolKind::AccessFunction,
                         origin(*name_expr, name), DeclarationRole::AccessName);
            declared_expressions_.insert(name_expr->id().value);
        } else if ((kind == ScopeKind::Discipline && (text == "potential" || text == "flow")) ||
                   (kind == ScopeKind::Nature && (text == "ddt_nature" || text == "idt_nature"))) {
            if (const auto name_expr = A::cast<A::NameExpr>(value->without_parens())) {
                roles_[name_expr->id().value] = ReferenceRole::NatureType;
            }
        } else if (kind == ScopeKind::Discipline && text == "domain") {
            if (const auto name_expr = A::cast<A::NameExpr>(value->without_parens())) {
                roles_[name_expr->id().value] = ReferenceRole::Domain;
            }
        }
    }

    void collect(const A::Root& root) {
        std::vector<Work> stack{{root, root_, std::nullopt}};
        while (!stack.empty()) {
            auto work = std::move(stack.back());
            stack.pop_back();
            const auto& current = work.node;
            auto scope = work.scope;
            parents_[current.id().value] = work.parent;
            if (const auto module = A::cast<A::Module>(current)) {
                if (const auto name = module->name()) {
                    const auto definition = define(scope, NameSpace::Type, *name, SymbolKind::Module, origin(current, *name));
                    scope = add_scope(ScopeKind::Module, canonical(*name), scope, current.id(), definition.id);
                    if (definition.created) result_.symbols[definition.id.value].inner_scope = scope;
                    close_label(current, scope, "endmodule", definition.id);
                }
            } else if (const auto function = A::cast<A::AnalogFunction>(current)) {
                if (const auto name = function->name()) {
                    const auto definition = define(scope, NameSpace::Value, *name, SymbolKind::Function, origin(current, *name));
                    scope = add_scope(ScopeKind::Function, canonical(*name), scope, current.id(), definition.id);
                    if (definition.created) result_.symbols[definition.id.value].inner_scope = scope;
                    const auto result = define(scope, NameSpace::Value, *name, SymbolKind::FunctionResult,
                                               origin(current, *name), DeclarationRole::ImplicitResult);
                    auto& data = result_.symbols[result.id.value];
                    if (result_.symbol(definition.id).kind == SymbolKind::Function) data.return_of = definition.id;
                    data.type_name = function->return_type() ?
                        canonical(*function->return_type()) : std::string("real");
                }
            } else if (const auto block = A::cast<A::BlockStmt>(current)) {
                std::optional<SymbolId> label;
                if (const auto name = block->label()) {
                    if (enclosing(scope, ScopeKind::Function)) {
                        diagnose(origin(current, *name), "HIR_FUNCTION_BLOCK", "analog functions cannot contain named blocks");
                    }
                    const auto definition = define(scope, NameSpace::Value, *name, SymbolKind::NamedBlock, origin(current, *name));
                    label = definition.id;
                    scope = add_scope(ScopeKind::NamedBlock, canonical(*name), scope, current.id(), definition.id);
                    if (definition.created) result_.symbols[definition.id.value].inner_scope = scope;
                }
                close_label(current, scope, "end", label);
            } else if (const auto nature = A::cast<A::NatureDecl>(current)) {
                if (const auto name = nature->name()) {
                    const auto definition = define(scope, NameSpace::Type, *name, SymbolKind::Nature, origin(current, *name));
                    scope = add_scope(ScopeKind::Nature, canonical(*name), scope, current.id(), definition.id);
                    if (definition.created) result_.symbols[definition.id.value].inner_scope = scope;
                    if (const auto parent = nature->parent_name()) {
                        pending_.push_back({origin(current, *parent), scope, NameSpace::Type, ReferenceRole::NatureType});
                    }
                }
            } else if (const auto discipline = A::cast<A::DisciplineDecl>(current)) {
                if (const auto name = discipline->name()) {
                    const auto definition = define(scope, NameSpace::Type, *name, SymbolKind::Discipline, origin(current, *name));
                    scope = add_scope(ScopeKind::Discipline, canonical(*name), scope, current.id(), definition.id);
                    if (definition.created) result_.symbols[definition.id.value].inner_scope = scope;
                }
            } else if (const auto port = A::cast<A::Port>(current)) {
                collect_port(*port, scope);
            } else if (const auto declaration = A::cast<A::Declaration>(current)) {
                collect_declaration(*declaration, scope);
            } else if (const auto branch = A::cast<A::BranchDecl>(current)) {
                if (result_.scope(scope).kind != ScopeKind::Module) {
                    diagnose(origin(current), "HIR_DECL_SCOPE", "branches must be declared in module scope");
                }
                for (const auto& name : branch->names()) {
                    (void)define(scope, NameSpace::Value, name, SymbolKind::Branch, origin(current, name));
                }
            } else if (const auto property = A::cast<A::PropertyDecl>(current)) {
                collect_property(*property, scope);
            }
            result_.node_scopes[current.id().value] = scope;
            const auto children = current.children();
            for (auto i = children.rbegin(); i != children.rend(); ++i) {
                stack.push_back({*i, scope, current.id()});
            }
        }
    }

    void mark_target(const A::Expr& expression, ReferenceRole role) {
        auto current = expression.without_parens();
        // a[i] 写入 a，但 i 仍是读取。成员引用由专门诊断处理。
        while (const auto subscript = A::cast<A::IndexExpr>(current)) {
            if (!subscript->base()) return;
            current = subscript->base()->without_parens();
        }
        if (A::NameExpr::can_cast(current.kind())) roles_[current.id().value] = role;
    }
    void classify() {
        for (std::size_t i = 0; i < result_.syntax.node_count(); ++i) {
            const auto current = node({i});
            if (const auto assignment = A::cast<A::AssignmentStmt>(current)) {
                if (const auto left = assignment->lhs()) {
                    const auto op = assignment->op();
                    mark_target(*left, op && op->text() == "=" ? ReferenceRole::Write : ReferenceRole::ReadWrite);
                }
            } else if (const auto unary = A::cast<A::UnaryExpr>(current)) {
                const auto op = unary->op();
                if (op && (op->text() == "++" || op->text() == "--") && unary->operand()) {
                    mark_target(*unary->operand(), ReferenceRole::ReadWrite);
                }
            } else if (const auto postfix = A::cast<A::PostfixExpr>(current)) {
                if (const auto value = postfix->operand()) mark_target(*value, ReferenceRole::ReadWrite);
            } else if (const auto call = A::cast<A::CallExpr>(current)) {
                const auto callee = call->callee();
                const auto name_expr = callee ? A::cast<A::NameExpr>(callee->without_parens()) : std::nullopt;
                if (!name_expr || !name_expr->name()) {
                    diagnose(origin(current), "HIR_UNSUPPORTED_CALLEE", "H1 supports simple named callees only");
                    continue;
                }
                roles_[name_expr->id().value] = ReferenceRole::Call;
                const auto target = lookup_call(result_.scope_of(current.id()), canonical(*name_expr->name()));
                if (target && result_.symbol(*target).kind == SymbolKind::AccessFunction) {
                    for (const auto& arg : call->arguments()) mark_target(arg, ReferenceRole::ProbeTerminal);
                }
            } else if (const auto branch = A::cast<A::BranchDecl>(current)) {
                for (const auto& terminal : branch->terminals()) mark_target(terminal, ReferenceRole::BranchTerminal);
            } else if (const auto jump = A::cast<A::JumpStmt>(current)) {
                if (jump->keyword() && jump->keyword()->text() == "disable" && jump->value()) {
                    mark_target(*jump->value(), ReferenceRole::BlockTarget);
                }
            } else if (current.kind() == SyntaxKind::MemberExpr) {
                diagnose(origin(current), "HIR_UNSUPPORTED_MEMBER", "hierarchical/member reference binding is not implemented");
            }
        }
        for (std::size_t i = 0; i < result_.syntax.node_count(); ++i) {
            const auto current = node({i});
            if (const auto name = A::cast<A::NameExpr>(current)) {
                if (declared_expressions_.contains(i) || !name->name()) continue;
                const auto role = roles_.contains(i) ? roles_.at(i) : ReferenceRole::Read;
                const auto space = role == ReferenceRole::NatureType ? NameSpace::Type : NameSpace::Value;
                pending_.push_back({origin(current, *name->name()), result_.scope_of(current.id()), space, role});
            }
        }
    }

    void resolve() {
        std::stable_sort(pending_.begin(), pending_.end(), [](const Pending& a, const Pending& b) {
            return a.origin.token_index < b.origin.token_index;
        });
        for (const auto& item : pending_) {
            const auto name = canonical(token(item.origin));
            auto target = item.forced ? item.target :
                item.role == ReferenceRole::Call ? lookup_call(item.scope, name) :
                                                  lookup(item.scope, item.name_space, name);
            Reference reference{name, item.role, item.name_space, item.scope, item.origin,
                                target, ReferenceStatus::Resolved};
            if (!target) {
                reference.status = item.forced ? ReferenceStatus::Forbidden : ReferenceStatus::Undefined;
                diagnose(item.origin, item.forced ? "HIR_END_LABEL" :
                    item.name_space == NameSpace::Type ? "HIR_UNDEFINED_TYPE" : "HIR_UNDEFINED",
                    item.forced ? "closing label has no corresponding opening name" :
                                  "undefined name '" + name + "'");
                result_.references.push_back(std::move(reference));
                continue;
            }
            const auto& definition = result_.symbol(*target);
            bool wrong_kind = false;
            if (item.role == ReferenceRole::Call) wrong_kind = !callable(definition.kind);
            if (item.role == ReferenceRole::DisciplineType) wrong_kind = definition.kind != SymbolKind::Discipline;
            if (item.role == ReferenceRole::NatureType) wrong_kind = definition.kind != SymbolKind::Nature;
            if (item.role == ReferenceRole::Domain) wrong_kind = definition.kind != SymbolKind::Domain;
            if (item.role == ReferenceRole::BranchTerminal) wrong_kind = definition.kind != SymbolKind::Net;
            if (item.role == ReferenceRole::ProbeTerminal) {
                wrong_kind = definition.kind != SymbolKind::Net && definition.kind != SymbolKind::Branch;
            }
            if (item.role == ReferenceRole::BlockTarget) wrong_kind = definition.kind != SymbolKind::NamedBlock;
            if (wrong_kind) {
                reference.status = ReferenceStatus::WrongKind;
                diagnose(item.origin, item.role == ReferenceRole::Call ? "HIR_NOT_CALLABLE" : "HIR_REFERENCE_KIND",
                         "name '" + name + "' resolves to " + std::string(symbol_kind_name(definition.kind)) +
                         ", which is invalid for " + std::string(reference_role_name(item.role)), first_site(*target));
            } else if (item.forced && name != definition.name) {
                reference.status = ReferenceStatus::Forbidden;
                diagnose(item.origin, "HIR_END_LABEL", "closing label '" + name + "' does not match '" +
                         definition.name + "'", first_site(*target));
            } else if (const auto function = enclosing(item.scope, ScopeKind::Function)) {
                if (!inside(definition.scope, *function) &&
                    (definition.kind == SymbolKind::Variable || definition.kind == SymbolKind::Net ||
                     definition.kind == SymbolKind::Branch)) {
                    reference.status = ReferenceStatus::Forbidden;
                    diagnose(item.origin, "HIR_FUNCTION_CAPTURE",
                             "analog function cannot capture module variable/net/branch '" + name + "'", first_site(*target));
                } else if (item.role == ReferenceRole::Call &&
                           (definition.kind == SymbolKind::AccessFunction ||
                            definition.builtin == BuiltinKind::AnalogOperator)) {
                    reference.status = ReferenceStatus::Forbidden;
                    diagnose(item.origin, "HIR_FUNCTION_ACCESS", "analog function cannot use this access/filter function");
                }
            }
            result_.references.push_back(std::move(reference));
        }
    }

    BindingResult result_;
    ScopeId prelude_, root_;
    std::vector<std::optional<NodeId>> parents_;
    std::map<std::size_t, ReferenceRole> roles_;
    std::map<std::size_t, PortState> port_states_;
    std::set<std::size_t> declared_expressions_;
    std::vector<Pending> pending_;
};

} // namespace

BindingResult bind_names(const SyntaxTree& tree) { return Binder(tree).run(); }

} // namespace vagrad::hir
