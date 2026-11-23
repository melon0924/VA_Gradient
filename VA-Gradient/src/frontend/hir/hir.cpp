#include "vagrad/frontend/hir/hir.hpp"

#include <algorithm>

namespace vagrad::hir {

std::optional<SymbolId> BindingResult::lookup(ScopeId from, NameSpace space, std::string_view name) const {
    auto current = std::optional<ScopeId>{from};
    while (current) {
        const auto& data = scope(*current);
        const auto& names = space == NameSpace::Value ? data.values : data.types;
        const auto found = names.find(std::string(name));
        if (found != names.end()) return found->second;
        current = data.parent;
    }
    return std::nullopt;
}
std::vector<SymbolId> BindingResult::declared_at(std::size_t token_index) const {
    std::vector<SymbolId> result;
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        const auto& sites = symbols[i].declarations;
        if (std::any_of(sites.begin(), sites.end(), [token_index](const auto& site) {
                return site.origin.token_index == token_index;
            })) result.push_back({i});
    }
    return result;
}
std::optional<ReferenceId> BindingResult::reference_at(std::size_t token_index) const {
    for (std::size_t i = 0; i < references.size(); ++i) {
        if (references[i].origin.token_index == token_index) return ReferenceId{i};
    }
    return std::nullopt;
}
bool BindingResult::has_errors() const {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == Severity::Error;
    });
}

#define HIR_ENUM_CASE(type, item) case type::item: return #item;
std::string_view name_space_name(NameSpace value) {
    switch (value) { HIR_ENUM_CASE(NameSpace, Value) HIR_ENUM_CASE(NameSpace, Type) }
    return "Unknown";
}
std::string_view scope_kind_name(ScopeKind value) {
    switch (value) {
        HIR_ENUM_CASE(ScopeKind, Prelude) HIR_ENUM_CASE(ScopeKind, CompilationUnit)
        HIR_ENUM_CASE(ScopeKind, Module) HIR_ENUM_CASE(ScopeKind, NamedBlock)
        HIR_ENUM_CASE(ScopeKind, Function) HIR_ENUM_CASE(ScopeKind, Nature)
        HIR_ENUM_CASE(ScopeKind, Discipline)
    }
    return "Unknown";
}
std::string_view symbol_kind_name(SymbolKind value) {
    switch (value) {
        HIR_ENUM_CASE(SymbolKind, Module) HIR_ENUM_CASE(SymbolKind, Nature)
        HIR_ENUM_CASE(SymbolKind, Discipline) HIR_ENUM_CASE(SymbolKind, Parameter)
        HIR_ENUM_CASE(SymbolKind, LocalParameter) HIR_ENUM_CASE(SymbolKind, AliasParameter)
        HIR_ENUM_CASE(SymbolKind, Variable) HIR_ENUM_CASE(SymbolKind, Net)
        HIR_ENUM_CASE(SymbolKind, Branch) HIR_ENUM_CASE(SymbolKind, Function)
        HIR_ENUM_CASE(SymbolKind, FunctionResult) HIR_ENUM_CASE(SymbolKind, NamedBlock)
        HIR_ENUM_CASE(SymbolKind, AccessFunction) HIR_ENUM_CASE(SymbolKind, BuiltinFunction)
        HIR_ENUM_CASE(SymbolKind, Environment) HIR_ENUM_CASE(SymbolKind, Domain)
    }
    return "Unknown";
}
std::string_view builtin_kind_name(BuiltinKind value) {
    switch (value) {
        HIR_ENUM_CASE(BuiltinKind, None) HIR_ENUM_CASE(BuiltinKind, Type)
        HIR_ENUM_CASE(BuiltinKind, Access) HIR_ENUM_CASE(BuiltinKind, Math)
        HIR_ENUM_CASE(BuiltinKind, Environment) HIR_ENUM_CASE(BuiltinKind, Diagnostic)
        HIR_ENUM_CASE(BuiltinKind, AnalogOperator) HIR_ENUM_CASE(BuiltinKind, Domain)
    }
    return "Unknown";
}
std::string_view declaration_role_name(DeclarationRole value) {
    switch (value) {
        HIR_ENUM_CASE(DeclarationRole, Definition) HIR_ENUM_CASE(DeclarationRole, PortHeader)
        HIR_ENUM_CASE(DeclarationRole, Direction) HIR_ENUM_CASE(DeclarationRole, DataType)
        HIR_ENUM_CASE(DeclarationRole, ImplicitResult) HIR_ENUM_CASE(DeclarationRole, AccessName)
    }
    return "Unknown";
}
std::string_view reference_role_name(ReferenceRole value) {
    switch (value) {
        HIR_ENUM_CASE(ReferenceRole, Read) HIR_ENUM_CASE(ReferenceRole, Write)
        HIR_ENUM_CASE(ReferenceRole, ReadWrite) HIR_ENUM_CASE(ReferenceRole, Call)
        HIR_ENUM_CASE(ReferenceRole, DisciplineType) HIR_ENUM_CASE(ReferenceRole, NatureType)
        HIR_ENUM_CASE(ReferenceRole, ProbeTerminal) HIR_ENUM_CASE(ReferenceRole, BranchTerminal)
        HIR_ENUM_CASE(ReferenceRole, Domain) HIR_ENUM_CASE(ReferenceRole, BlockTarget)
        HIR_ENUM_CASE(ReferenceRole, EndLabel)
    }
    return "Unknown";
}
std::string_view reference_status_name(ReferenceStatus value) {
    switch (value) {
        HIR_ENUM_CASE(ReferenceStatus, Resolved) HIR_ENUM_CASE(ReferenceStatus, Undefined)
        HIR_ENUM_CASE(ReferenceStatus, WrongKind) HIR_ENUM_CASE(ReferenceStatus, Forbidden)
    }
    return "Unknown";
}
std::string_view port_direction_name(PortDirection value) {
    switch (value) {
        HIR_ENUM_CASE(PortDirection, Input) HIR_ENUM_CASE(PortDirection, Output)
        HIR_ENUM_CASE(PortDirection, Inout)
    }
    return "Unknown";
}
#undef HIR_ENUM_CASE

} // namespace vagrad::hir
