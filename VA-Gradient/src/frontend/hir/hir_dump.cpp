#include "vagrad/frontend/hir/hir.hpp"

#include <algorithm>
#include <iomanip>
#include <ostream>

namespace vagrad::hir {
namespace {

void print_origin(std::ostream& out, const BindingResult& result, Origin source) {
    const auto& token = result.syntax.input().tokens.at(source.token_index);
    const auto span = token.original.span;
    const auto location = result.syntax.input().files.at(span.file.value)->locate(span.begin);
    out << " Node[" << source.node.value << "] Token[" << source.token_index
        << "] Expanded[" << token.begin << ',' << token.end << ") Origin(File[" << span.file.value
        << "] " << location.line << ':' << location.column << " Context[" << token.context << "])";
}

} // namespace

void print_bindings(std::ostream& out, const BindingResult& result) {
    out << "HIR-H1: scopes, symbols and name bindings\n";
    print_source_map(out, result.syntax.input());
    for (std::size_t i = 0; i < result.scopes.size(); ++i) {
        const auto& scope = result.scopes[i];
        out << "Scope[" << i << "] " << scope_kind_name(scope.kind) << ' ' << std::quoted(scope.name);
        if (scope.parent) out << " Parent[" << scope.parent->value << ']';
        if (scope.owner_node) out << " Node[" << scope.owner_node->value << ']';
        if (scope.owner_symbol) out << " OwnerSymbol[" << scope.owner_symbol->value << ']';
        out << '\n';
    }
    std::size_t builtins = 0;
    for (std::size_t i = 0; i < result.symbols.size(); ++i) {
        const auto& symbol = result.symbols[i];
        out << "Symbol[" << i << "] " << symbol_kind_name(symbol.kind) << ' ' << std::quoted(symbol.name)
            << " Scope[" << symbol.scope.value << "] Namespace=" << name_space_name(symbol.name_space);
        if (symbol.builtin != BuiltinKind::None) {
            ++builtins;
            out << " Builtin=" << builtin_kind_name(symbol.builtin);
        }
        if (symbol.is_port) out << " port=true";
        if (symbol.direction) out << " direction=" << port_direction_name(*symbol.direction);
        if (symbol.type_name) out << " declared_type=" << std::quoted(*symbol.type_name);
        if (symbol.inner_scope) out << " InnerScope[" << symbol.inner_scope->value << ']';
        if (symbol.return_of) out << " ReturnOfSymbol[" << symbol.return_of->value << ']';
        out << '\n';
        for (const auto& site : symbol.declarations) {
            out << "  Declaration " << declaration_role_name(site.role);
            if (!site.detail.empty()) out << ' ' << std::quoted(site.detail);
            print_origin(out, result, site.origin);
            out << '\n';
        }
    }
    std::size_t resolved = 0;
    for (std::size_t i = 0; i < result.references.size(); ++i) {
        const auto& reference = result.references[i];
        out << "Ref[" << i << "] " << reference_role_name(reference.role) << ' '
            << std::quoted(reference.name) << " Scope[" << reference.scope.value << ']';
        if (reference.target) {
            out << " -> Symbol[" << reference.target->value << "] "
                << symbol_kind_name(result.symbol(*reference.target).kind);
        } else {
            out << " -> <unresolved>";
        }
        out << " Status=" << reference_status_name(reference.status);
        print_origin(out, result, reference.origin);
        out << '\n';
        if (reference.status == ReferenceStatus::Resolved) ++resolved;
    }
    const auto errors = std::count_if(result.diagnostics.begin(), result.diagnostics.end(),
        [](const Diagnostic& diagnostic) { return diagnostic.severity == Severity::Error; });
    out << "Summary: scopes=" << result.scopes.size()
        << " user_symbols=" << result.symbols.size() - builtins
        << " builtin_symbols=" << builtins
        << " references=" << result.references.size()
        << " resolved=" << resolved << " errors=" << errors << '\n';
    for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
        const auto& diagnostic = result.diagnostics[i];
        out << "Diagnostic[" << i << "] " << diagnostic.code << ' ' << std::quoted(diagnostic.message) << '\n';
    }
}

} // namespace vagrad::hir
