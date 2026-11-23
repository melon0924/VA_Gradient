#pragma once

#include "vagrad/frontend/syntax/syntax_tree.hpp"

#include <iosfwd>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vagrad::hir {

struct ScopeId {
    std::size_t value = std::numeric_limits<std::size_t>::max();
    friend bool operator==(ScopeId, ScopeId) = default;
};
struct SymbolId {
    std::size_t value = std::numeric_limits<std::size_t>::max();
    friend bool operator==(SymbolId, SymbolId) = default;
};
struct ReferenceId {
    std::size_t value = std::numeric_limits<std::size_t>::max();
    friend bool operator==(ReferenceId, ReferenceId) = default;
};

enum class NameSpace { Value, Type };
enum class ScopeKind { Prelude, CompilationUnit, Module, NamedBlock, Function, Nature, Discipline };
enum class SymbolKind {
    Module, Nature, Discipline, Parameter, LocalParameter, AliasParameter,
    Variable, Net, Branch, Function, FunctionResult, NamedBlock,
    AccessFunction, BuiltinFunction, Environment, Domain
};
enum class BuiltinKind { None, Type, Access, Math, Environment, Diagnostic, AnalogOperator, Domain };
enum class DeclarationRole { Definition, PortHeader, Direction, DataType, ImplicitResult, AccessName };
enum class ReferenceRole {
    Read, Write, ReadWrite, Call, DisciplineType, NatureType,
    ProbeTerminal, BranchTerminal, Domain, BlockTarget, EndLabel
};
enum class ReferenceStatus { Resolved, Undefined, WrongKind, Forbidden };
enum class PortDirection { Input, Output, Inout };

// 所有来源引用既有 CST / 展开 token，不复制成脱离宏上下文的文本。
struct Origin {
    NodeId node;
    std::size_t token_index;
};
struct DeclarationSite {
    DeclarationRole role;
    Origin origin;
    std::string detail;
};
struct Scope {
    ScopeKind kind;
    std::string name;
    std::optional<ScopeId> parent;
    std::optional<NodeId> owner_node;
    std::optional<SymbolId> owner_symbol;
    std::vector<ScopeId> children;
    std::map<std::string, SymbolId> values;
    std::map<std::string, SymbolId> types;
};
struct Symbol {
    std::string name; // 规范化名称；转义标识符去掉开头反斜杠
    SymbolKind kind;
    NameSpace name_space;
    ScopeId scope;
    BuiltinKind builtin = BuiltinKind::None;
    std::vector<DeclarationSite> declarations;
    std::optional<ScopeId> inner_scope;
    std::optional<SymbolId> return_of;
    bool is_port = false;
    std::optional<PortDirection> direction;
    std::optional<std::string> type_name; // H1 保存声明文字，H2 再绑定完整类型
};
struct Reference {
    std::string name;
    ReferenceRole role;
    NameSpace name_space;
    ScopeId scope;
    Origin origin;
    std::optional<SymbolId> target;
    ReferenceStatus status = ReferenceStatus::Undefined;
};

// H1 的语义交付：作用域、定义、引用和诊断；尚不含 H2 的有类型表达式。
struct BindingResult {
    explicit BindingResult(SyntaxTree source) : syntax(std::move(source)) {}
    SyntaxTree syntax;
    std::vector<Scope> scopes;
    std::vector<Symbol> symbols;
    std::vector<Reference> references;
    std::vector<ScopeId> node_scopes;
    std::vector<Diagnostic> diagnostics;

    const Scope& scope(ScopeId id) const { return scopes.at(id.value); }
    const Symbol& symbol(SymbolId id) const { return symbols.at(id.value); }
    const Reference& reference(ReferenceId id) const { return references.at(id.value); }
    ScopeId scope_of(NodeId node) const { return node_scopes.at(node.value); }
    // 原始词法向上查询，不执行调用上下文/定义先后/函数捕获限制。
    std::optional<SymbolId> lookup(ScopeId from, NameSpace space, std::string_view name) const;
    std::vector<SymbolId> declared_at(std::size_t token_index) const;
    std::optional<ReferenceId> reference_at(std::size_t token_index) const;
    bool has_errors() const;
};

// 调用者应先检查 ParseResult.has_errors()；正式 CLI 会阻止错误语法进入 H1。
BindingResult bind_names(const SyntaxTree& tree);
void print_bindings(std::ostream& out, const BindingResult& result);

std::string_view name_space_name(NameSpace value);
std::string_view scope_kind_name(ScopeKind value);
std::string_view symbol_kind_name(SymbolKind value);
std::string_view builtin_kind_name(BuiltinKind value);
std::string_view declaration_role_name(DeclarationRole value);
std::string_view reference_role_name(ReferenceRole value);
std::string_view reference_status_name(ReferenceStatus value);
std::string_view port_direction_name(PortDirection value);

} // namespace vagrad::hir
