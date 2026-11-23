#include "vagrad/frontend/hir/hir.hpp"
#include "vagrad/frontend/ast/ast.hpp"
#include "vagrad/frontend/parser/parser.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using namespace vagrad;
namespace H = vagrad::hir;
namespace A = vagrad::ast;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
H::BindingResult from_text(std::string source) {
    SourceManager sources;
    const auto id = sources.add_virtual("<hir-test.va>", std::move(source));
    const auto parsed = parse(std::make_shared<const PreprocessResult>(preprocess(sources, id)));
    if (parsed.has_errors()) {
        for (const auto& error : parsed.diagnostics) std::cerr << error.code << ": " << error.message << '\n';
    }
    require(!parsed.has_errors(), "H1 fixture must have valid frontend syntax");
    return H::bind_names(build_syntax_tree(parsed));
}
bool has_code(const H::BindingResult& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& item) { return item.code == code; });
}
H::ScopeId scope_named(const H::BindingResult& result, H::ScopeKind kind, std::string_view name) {
    for (std::size_t i = 0; i < result.scopes.size(); ++i) {
        if (result.scopes[i].kind == kind && result.scopes[i].name == name) return {i};
    }
    throw std::runtime_error("missing scope " + std::string(name));
}
H::SymbolId local(const H::BindingResult& result, H::ScopeId scope, std::string_view name,
                  H::NameSpace space = H::NameSpace::Value) {
    const auto& data = result.scope(scope);
    const auto& names = space == H::NameSpace::Value ? data.values : data.types;
    const auto found = names.find(std::string(name));
    require(found != names.end(), "missing local symbol " + std::string(name));
    return found->second;
}
void invariants(const H::BindingResult& result) {
    const auto& input = result.syntax.input();
    require(result.node_scopes.size() == result.syntax.node_count(), "all nodes have a scope");
    for (const auto scope : result.node_scopes) require(scope.value < result.scopes.size(), "valid node scope");
    for (std::size_t i = 0; i < result.scopes.size(); ++i) {
        const auto& scope = result.scopes[i];
        if (scope.parent) {
            require(scope.parent->value < i, "scope parent precedes child");
            const auto& siblings = result.scope(*scope.parent).children;
            require(std::count(siblings.begin(), siblings.end(), H::ScopeId{i}) == 1, "scope parent/child links");
        }
        if (scope.owner_node) require(scope.owner_node->value < result.syntax.node_count(), "valid scope owner node");
        if (scope.owner_symbol) require(scope.owner_symbol->value < result.symbols.size(), "valid scope owner symbol");
        for (const auto space : {H::NameSpace::Value, H::NameSpace::Type}) {
            const auto& names = space == H::NameSpace::Value ? scope.values : scope.types;
            for (const auto& [name, id] : names) {
                const auto& symbol = result.symbol(id);
                require(symbol.name == name && symbol.scope == H::ScopeId{i} && symbol.name_space == space,
                        "scope lookup points to its own definition");
            }
        }
    }
    for (const auto& symbol : result.symbols) {
        const auto& scope = result.scope(symbol.scope);
        const auto& names = symbol.name_space == H::NameSpace::Value ? scope.values : scope.types;
        require(names.contains(symbol.name), "every symbol is indexed");
        require(symbol.builtin != H::BuiltinKind::None || !symbol.declarations.empty(),
                "user definition has an origin");
        for (const auto& site : symbol.declarations) {
            require(site.origin.node.value < result.syntax.node_count() &&
                    site.origin.token_index < input.tokens.size(), "valid declaration origin");
        }
    }
    std::set<std::size_t> occurrences;
    std::size_t previous = 0;
    for (const auto& reference : result.references) {
        require(reference.origin.node.value < result.syntax.node_count() &&
                reference.origin.token_index < input.tokens.size(), "valid reference origin");
        require(reference.scope == result.scope_of(reference.origin.node), "reference uses node lexical scope");
        require(reference.origin.token_index >= previous, "references printed in source order");
        previous = reference.origin.token_index;
        require(occurrences.insert(previous).second, "one reference per token occurrence");
        if (reference.target) require(reference.target->value < result.symbols.size(), "valid target SymbolId");
        if (reference.status == H::ReferenceStatus::Resolved) require(reference.target.has_value(), "resolved means defined");
        require(result.reference_at(previous).has_value(), "reference query by token");
    }
    if (!result.has_errors()) {
        for (std::size_t i = 0; i < result.syntax.node_count(); ++i) {
            const A::Node node(result.syntax, {i});
            if (const auto name = A::cast<A::NameExpr>(node); name && name->name()) {
                const auto index = name->name()->index();
                require(result.reference_at(index) || !result.declared_at(index).empty(),
                        "each NameExpr is a bound use or an access-function declaration");
            }
        }
    }
}

void model() {
    for (const bool soft : {false, true}) {
        SourceManager sources;
        PreprocessOptions options;
        if (soft) options.defines = {{"SOFT_THRESHOLD","1"},{"TRACE_MOS","1"}};
        const auto file = sources.load(std::filesystem::path(VAGRAD_TEST_DATA_DIR) / "simple_mos.va");
        const auto parsed = parse(std::make_shared<const PreprocessResult>(preprocess(sources, file, options)));
        require(!parsed.has_errors(), "model frontend");
        const auto tree = build_syntax_tree(parsed);
        const auto before = tree.text(tree.root());
        const auto result = H::bind_names(tree);
        require(!result.has_errors(), "default/soft model H1 binding");
        require(&tree.node(tree.root()) == &result.syntax.node(result.syntax.root()),
                "H1 shares existing CST storage");
        require(result.syntax.text(result.syntax.root()) == before, "H1 preserves expanded source");
        const auto module = scope_named(result, H::ScopeKind::Module, "simple_mos");
        const auto block = scope_named(result, H::ScopeKind::NamedBlock, "evaluate_current");
        const auto d = local(result, module, "d");
        const auto& port = result.symbol(d);
        require(port.kind == H::SymbolKind::Net && port.is_port &&
                port.direction == H::PortDirection::Inout && port.type_name == "electrical" &&
                port.declarations.size() == 3, "header/direction/discipline merge to one net");
        require(local(result,module,"G0") == result.lookup(block,H::NameSpace::Value,"G0"),
                "block resolves module parameter");
        const auto ids = local(result,module,"ids");
        require(result.symbol(ids).kind == H::SymbolKind::Variable, "ids symbol");
        std::size_t writes = 0, reads = 0, copied = 0;
        std::vector<const H::Reference*> square;
        for (const auto& reference : result.references) {
            require(reference.status == H::ReferenceStatus::Resolved, "all model references resolve");
            if (reference.target == ids) {
                writes += reference.role == H::ReferenceRole::Write ? 1 : 0;
                reads += reference.role == H::ReferenceRole::Read ? 1 : 0;
            }
            const auto& token = result.syntax.input().tokens[reference.origin.token_index];
            const auto& context = result.syntax.input().contexts[token.context];
            if (context.kind == ExpansionKind::Argument && context.macro_name == "SQUARE") {
                square.push_back(&reference);
                ++copied;
            }
        }
        require(writes == 3 && reads == (soft ? 2 : 1), "ids assignments bind one definition");
        require(copied == 4 && square[0]->target == square[1]->target &&
                square[0]->origin.token_index != square[1]->origin.token_index &&
                square[0]->origin.node != square[1]->origin.node,
                "macro copies share a symbol and retain distinct occurrences");
        const auto max_name = soft ? "sqrt" : "max";
        require(std::any_of(result.references.begin(),result.references.end(),[&](const auto& ref) {
            return ref.name == max_name && ref.role == H::ReferenceRole::Call && ref.target &&
                   result.symbol(*ref.target).builtin == H::BuiltinKind::Math;
        }), "math builtin is explicit");
        std::ostringstream out;
        H::print_bindings(out,result);
        require(out.str().find("HIR-H1") != std::string::npos &&
                out.str().find("-> Symbol[") != std::string::npos, "inspectable bindings");
        invariants(result);
    }
}

void scopes_and_shadowing() {
    const auto result = from_text(
        "module m; parameter real K=1; real ids,x;\n"
        "analog begin : outer\n"
        "real x; x=K;\n"
        "begin : inner real K; K=x; ids=K; disable outer; end : inner\n"
        "ids=x+K;\n"
        "end : outer\nendmodule : m\n");
    require(!result.has_errors(), "named scopes and closing labels");
    const auto module = scope_named(result,H::ScopeKind::Module,"m");
    const auto outer = scope_named(result,H::ScopeKind::NamedBlock,"outer");
    const auto inner = scope_named(result,H::ScopeKind::NamedBlock,"inner");
    const auto global_k = local(result,module,"K");
    const auto inner_k = local(result,inner,"K");
    const auto outer_x = local(result,outer,"x");
    require(global_k != inner_k && outer_x != local(result,module,"x"), "shadowing creates distinct symbols");
    for (const auto& ref : result.references) {
        if (ref.name == "K") {
            require(ref.target == (ref.scope == inner ? inner_k : global_k), "nearest definition wins");
        }
        if (ref.name == "x") require(ref.target == outer_x, "inner scope sees nearest ancestor x");
        if (ref.role == H::ReferenceRole::BlockTarget) {
            require(ref.target == local(result,module,"outer"), "disable resolves named block");
        }
    }
    require(result.lookup(inner,H::NameSpace::Value,"ids") == local(result,module,"ids"), "upward lookup");
    invariants(result);

    const auto anonymous = from_text("module m; real x; analog begin if(1) begin x=1; end end endmodule");
    require(!anonymous.has_errors() && anonymous.scopes.size() == 3, "unnamed begin/end do not create scopes");
    invariants(anonymous);
}

void functions() {
    const auto result = from_text(
        "module m; parameter real P=2; real x,y;\n"
        "analog function real f; input x; real x; parameter real P=3;\n"
        "begin f=g(x)+P; end endfunction\n"
        "analog function real g; input x; real x; begin g=x*P; end endfunction\n"
        "analog begin x=P; y=f(x); end endmodule\n");
    require(!result.has_errors(), "function arguments, result variables and forward function call");
    const auto module = scope_named(result,H::ScopeKind::Module,"m");
    const auto f = scope_named(result,H::ScopeKind::Function,"f");
    const auto g = scope_named(result,H::ScopeKind::Function,"g");
    const auto f_symbol = local(result,module,"f");
    const auto f_result = local(result,f,"f");
    require(result.symbol(f_result).kind == H::SymbolKind::FunctionResult &&
            result.symbol(f_result).return_of == f_symbol, "implicit return variable");
    const auto& argument = result.symbol(local(result,f,"x"));
    require(argument.is_port && argument.kind == H::SymbolKind::Variable &&
            argument.direction == H::PortDirection::Input && argument.type_name == "real" &&
            argument.declarations.size() == 2, "input and real are one formal argument");
    for (const auto& ref : result.references) {
        if (ref.name == "f") {
            require(ref.target == (ref.role == H::ReferenceRole::Call ? f_symbol : f_result),
                    "function callee differs from return variable");
        }
        if (ref.name == "P" && ref.scope == f) require(ref.target == local(result,f,"P"), "local parameter shadows module");
        if (ref.name == "P" && ref.scope == g) require(ref.target == local(result,module,"P"), "module parameter visible in function");
    }
    invariants(result);
    const auto capture = from_text(
        "module m; real outside; analog function real f; input a; real a; begin f=a+outside; end endfunction endmodule");
    require(has_code(capture,"HIR_FUNCTION_CAPTURE") && capture.diagnostics[0].related.size() >= 1,
            "analog function cannot capture module variable");
    invariants(capture);
    const auto access = from_text(
        "module m(d); inout d; electrical d;\n"
        "analog function real f; input a; real a; begin f=V(d); end endfunction endmodule");
    require(has_code(access,"HIR_FUNCTION_ACCESS"), "access functions forbidden inside analog function");
    invariants(access);
}

void types_accessors_and_names() {
    const auto ansi = from_text(
        "module ansi(input real a,b,output c,d,input electrical e,f); endmodule");
    require(!ansi.has_errors(), "ANSI port group inheritance");
    const auto ansi_scope = scope_named(ansi,H::ScopeKind::Module,"ansi");
    for (const auto name : {"a","b"}) {
        const auto& symbol = ansi.symbol(local(ansi,ansi_scope,name));
        require(symbol.kind == H::SymbolKind::Variable && symbol.direction == H::PortDirection::Input &&
                symbol.type_name == "real", "real port type/direction inherit within a group");
    }
    for (const auto name : {"c","d"}) {
        const auto& symbol = ansi.symbol(local(ansi,ansi_scope,name));
        require(symbol.kind == H::SymbolKind::Net && symbol.direction == H::PortDirection::Output &&
                !symbol.type_name, "new direction resets previous type");
    }
    require(ansi.symbol(local(ansi,ansi_scope,"f")).type_name == "electrical", "discipline inheritance");
    invariants(ansi);
    const auto wires = from_text("module m; wire a; wreal b; analog V(a,b); endmodule");
    require(!wires.has_errors(), "wire/wreal are net symbols");
    invariants(wires);

    const auto result = from_text(
        "nature Voltage; units=\"V\"; access=V; endnature\n"
        "nature Current; units=\"A\"; access=I; endnature\n"
        "nature Derived:Voltage; units=\"V\"; ddt_nature=Current; endnature\n"
        "discipline electrical; potential Voltage; flow Current; domain continuous; enddiscipline\n"
        "module m(d); inout d; electrical d; analog I(d)<+V(d); endmodule\n");
    require(!result.has_errors(), "explicit nature/discipline and access declarations");
    const auto root = scope_named(result,H::ScopeKind::CompilationUnit,"<root>");
    const auto module = scope_named(result,H::ScopeKind::Module,"m");
    const auto global_v = local(result,root,"V");
    require(result.symbol(global_v).kind == H::SymbolKind::AccessFunction &&
            result.symbol(global_v).builtin == H::BuiltinKind::None, "explicit access shadows prelude");
    for (const auto& ref : result.references) {
        if (ref.name == "V") require(ref.target == global_v, "user accessor is called");
        if (ref.role == H::ReferenceRole::DisciplineType) {
            require(ref.target == local(result,root,"electrical",H::NameSpace::Type), "explicit discipline resolves");
        }
    }
    require(result.lookup(module,H::NameSpace::Type,"Voltage") == local(result,root,"Voltage",H::NameSpace::Type),
            "nature names use type namespace");
    invariants(result);

    const auto escaped = from_text("module m; real abc; analog begin \\abc = abc; end endmodule");
    require(!escaped.has_errors(), "escaped and ordinary identifier equivalence");
    require(escaped.references.size() == 2 && escaped.references[0].target == escaped.references[1].target &&
            escaped.references[0].name == "abc", "canonical escaped name");
    invariants(escaped);

    const auto late = from_text(
        "module early(a); inout a; electrical a; analog Q(a); endmodule\n"
        "nature Charge; access=Q; units=\"C\"; endnature\n"
        "module late(a); inout a; electrical a; analog Q(a); endmodule\n");
    require(has_code(late,"HIR_UNDEFINED"), "access declared after earlier module is not visible there");
    std::vector<H::ReferenceStatus> statuses;
    for (const auto& ref : late.references) if (ref.name == "Q") statuses.push_back(ref.status);
    require(statuses.size() == 2 && statuses[0] == H::ReferenceStatus::Undefined &&
            statuses[1] == H::ReferenceStatus::Resolved, "access declaration order");
    invariants(late);
}

void read_write_and_diagnostics() {
    const auto rw = from_text(
        "module m; real a[0:3]; integer i; analog begin i=0; a[i]=1; a[i]+=2; ++i; i--; end endmodule");
    require(!rw.has_errors(), "index expressions and updates bind");
    std::vector<H::ReferenceRole> a_roles, i_roles;
    for (const auto& ref : rw.references) {
        if (ref.name == "a") a_roles.push_back(ref.role);
        if (ref.name == "i") i_roles.push_back(ref.role);
    }
    require(a_roles == std::vector<H::ReferenceRole>{H::ReferenceRole::Write,H::ReferenceRole::ReadWrite} &&
            i_roles == std::vector<H::ReferenceRole>{H::ReferenceRole::Write,H::ReferenceRole::Read,
                H::ReferenceRole::Read,H::ReferenceRole::ReadWrite,H::ReferenceRole::ReadWrite},
            "array index reads are distinct from base writes");
    invariants(rw);

    const std::vector<std::pair<std::string,std::string>> cases{
        {"module m; real x; real x; endmodule", "HIR_DUPLICATE"},
        {"module m; parameter real P=1,P=2; endmodule", "HIR_DUPLICATE"},
        {"module m; real foo,\\foo ; endmodule", "HIR_DUPLICATE"},
        {"module m(d); input d; output d; endmodule", "HIR_PORT_CONFLICT"},
        {"module m(d); inout d; electrical d; real d; endmodule", "HIR_PORT_CONFLICT"},
        {"module m(d); inout d; inout d; endmodule", "HIR_REDECLARATION"},
        {"module m(d); electrical d; electrical d; endmodule", "HIR_REDECLARATION"},
        {"module m; inout d; endmodule", "HIR_PORT_HEADER"},
        {"module m; real f; analog f(1); endmodule", "HIR_NOT_CALLABLE"},
        {"module m(d); inout d; electrical d; real V; analog V(d); endmodule", "HIR_NOT_CALLABLE"},
        {"module m; analog missing=1; endmodule", "HIR_UNDEFINED"},
        {"module m; unknown_type d; endmodule", "HIR_UNDEFINED_TYPE"},
        {"module m; m d; endmodule", "HIR_REFERENCE_KIND"},
        {"module m; real obj,ids; analog ids=obj.member; endmodule", "HIR_UNSUPPORTED_MEMBER"},
        {"module m; analog begin: a ; end:b endmodule", "HIR_END_LABEL"},
        {"module m; analog begin ; end:a endmodule", "HIR_END_LABEL"},
        {"module m; real b; analog begin:b ; end endmodule", "HIR_DUPLICATE"},
        {"module m; analog begin:b electrical d; end endmodule", "HIR_DECL_SCOPE"},
        {"module m; analog begin:b branch (0,0) ds; end endmodule", "HIR_DECL_SCOPE"},
        {"module m; analog function real f; input a; real a; begin:b f=a; end endfunction endmodule", "HIR_FUNCTION_BLOCK"},
        {"module m; analog begin real x; x=0; end endmodule", "HIR_DECL_SCOPE"}
    };
    for (const auto& [source,code] : cases) {
        const auto result = from_text(source);
        require(result.has_errors() && has_code(result,code), "expected diagnostic " + code + " for: " + source);
        invariants(result);
    }
    const auto macro = from_text(
        "`define BAD(x) ((x)+missing)\nmodule m; real y; analog y=`BAD(1); endmodule");
    require(has_code(macro,"HIR_UNDEFINED") && macro.diagnostics[0].related.size() >= 2,
            "undefined name in macro carries call/definition context");
    invariants(macro);
}

void lifetime_and_empty() {
    const auto retained = [] {
        return from_text("module kept; parameter real P=1; real y; analog y=P; endmodule");
    }();
    require(!retained.has_errors(), "binding lifetime");
    const auto module = scope_named(retained,H::ScopeKind::Module,"kept");
    require(retained.symbol(local(retained,module,"P")).name == "P" &&
            retained.syntax.text(retained.syntax.root()).find("y=P") != std::string::npos,
            "bindings own source/CST snapshots after local objects leave");
    invariants(retained);
    const auto empty = from_text("// no model\n");
    require(!empty.has_errors() && empty.references.empty(), "empty compilation unit");
    invariants(empty);

    SourceManager sources;
    const auto file = sources.add_virtual("<bad-syntax.va>","module m; analog x=; endmodule");
    const auto parsed = parse(std::make_shared<const PreprocessResult>(preprocess(sources,file)));
    require(parsed.has_errors(), "invalid syntax fixture");
    const auto blocked = H::bind_names(build_syntax_tree(parsed));
    require(has_code(blocked,"HIR_SYNTAX"), "library rejects ErrorNode syntax");
    invariants(blocked);
}

} // namespace

int main() {
    try {
        model();
        scopes_and_shadowing();
        functions();
        types_accessors_and_names();
        read_write_and_diagnostics();
        lifetime_and_empty();
        std::cout << "H1 scopes, declarations, binding, diagnostics, provenance and lifetime passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
