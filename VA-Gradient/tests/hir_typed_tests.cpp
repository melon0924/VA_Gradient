#include "vagrad/frontend/hir/eval.hpp"
#include "vagrad/frontend/parser/parser.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace vagrad;
namespace H = vagrad::hir;
void require(bool ok, const std::string& text) { if (!ok) throw std::runtime_error(text); }
bool code(const std::vector<Diagnostic>& ds, std::string_view text) {
    return std::any_of(ds.begin(), ds.end(), [=](const auto& d) { return d.code == text; });
}
H::HirModule from_source(SourceManager& sources, FileId id, const PreprocessOptions& options = {}) {
    const auto parsed = parse(std::make_shared<const PreprocessResult>(preprocess(sources, id, options)));
    require(!parsed.has_errors(), "valid test syntax");
    auto bindings = std::make_shared<const H::BindingResult>(H::bind_names(build_syntax_tree(parsed)));
    if (bindings->has_errors()) for (const auto& d : bindings->diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    require(!bindings->has_errors(), "valid H1 fixture");
    return H::lower_hir(std::move(bindings));
}
H::HirModule text(std::string value) {
    SourceManager sources;
    const auto id = sources.add_virtual("<typed-test.va>", std::move(value));
    return from_source(sources, id);
}
H::HirModule sample(const PreprocessOptions& options = {}) {
    SourceManager sources;
    const auto id = sources.load(std::filesystem::path(VAGRAD_TEST_DATA_DIR) / "simple_mos.va");
    return from_source(sources, id, options);
}
const H::ParamInfo& parameter(const H::HirModule& m, std::string_view name) {
    const auto it = std::find_if(m.parameters.begin(), m.parameters.end(),
        [&](const auto& p) { return m.bindings->symbol(p.symbol).name == name; });
    require(it != m.parameters.end(), "parameter exists");
    return *it;
}
double value(const H::HirModule& m, const H::ParamEnv& p, std::string_view name) {
    const auto& info = parameter(m, name);
    require(p.values[info.symbol.value].has_value(), "effective value exists");
    return H::number(*p.values[info.symbol.value]);
}
void close(double actual, double expected, const std::string& name) {
    require(std::abs(actual - expected) <= 1e-18 + 1e-12 * std::abs(expected), name + " numerical mismatch");
}
void test_literals() {
    struct Case { std::string text; double expected; H::Type type; };
    for (const auto& c : std::vector<Case>{{"1m",.001,H::Type::Real},{"1u",1e-6,H::Type::Real},
         {"3.0e2",300,H::Type::Real},{"8'h0F",15,H::Type::Integer},{"8'shFF",-1,H::Type::Integer},
         {"16'b1010_0011",163,H::Type::Integer},{"12'o77",63,H::Type::Integer},
         {"1M",1e6,H::Type::Real},{"1a",1e-18,H::Type::Real}}) {
        H::LiteralFormat format; std::string error;
        const auto result = H::decode_number(c.text, format, error);
        require(result && H::scalar_type(*result) == c.type, "literal type " + c.text);
        close(H::number(*result), c.expected, c.text);
        require(format.spelling == c.text, "raw spelling retained");
    }
    for (const auto& s : {"8'hxF", "4'hFF", "33'h1", "0'h1", "32'hFFFFFFFF", "1uMORE", "1.2.3", "1e9999", "2147483648"}) {
        H::LiteralFormat format; std::string error;
        require(!H::decode_number(s, format, error) && !error.empty(), "invalid literal rejected: " + std::string(s));
    }
}
void test_metadata_and_parameters() {
    const auto m = sample();
    require(!m.has_errors() && m.parameters.size() == 7, "sample typed");
    const auto env = H::resolve_parameters(m);
    require(!env.has_errors(), "sample defaults valid");
    close(value(m,env,"G0"),.001,"gain");
    close(value(m,env,"L"),1e-6,"length");
    close(value(m,env,"TRACE_MASK"),15,"based integer");
    require(parameter(m,"G0").type == H::Type::Real && parameter(m,"ENABLE").type == H::Type::Integer, "parameter types");
    require(parameter(m,"G0").attributes.at("units").text.find("\"A/V^2\"") != std::string::npos, "raw attribute retained");
    const auto& range = parameter(m,"G0").constraints.front();
    require(!range.lower_inclusive && range.upper.kind == H::Bound::Kind::PositiveInfinity, "open infinity constraint");
    for (const auto& [name, val] : std::map<std::string,double>{{"G0",0},{"L",-1},{"VTH0",-1},{"ENABLE",2},{"TNOM",0}})
        require(code(H::resolve_parameters(m,{{name,val}}).diagnostics,"EVAL_PARAMETER_RANGE"), "range rejects " + name);
    require(code(H::resolve_parameters(m,{{"G00",.1}}).diagnostics,"EVAL_PARAMETER"), "unknown override");
    const auto effective = H::resolve_parameters(m,{{"G0",.002},{"VTH0",.6},{"TNOM",600.0}});
    require(!effective.has_errors(), "valid sample overrides");
    close(value(m,effective,"G0"),.002,"override");
    close(value(m,env,"G0"),.001,"default immutable");
    const auto& default_expr = m.expr(*parameter(m,"G0").default_value);
    require(default_expr.type == H::Type::Real, "default expression retained");
    require(std::any_of(m.expressions.begin(),m.expressions.end(),[&](const auto& e) {
        return e.kind == H::ExprKind::Ref && e.symbol == parameter(m,"G0").symbol;
    }), "parameter use remains Ref, not folded default");
    for (const auto& e : m.expressions) {
        require(e.origin.node.value < m.bindings->syntax.node_count(), "expression origin valid");
        require(e.origin.token_index < m.bindings->syntax.input().tokens.size(), "expression token valid");
        for (const auto operand : e.operands) require(operand.value < m.expressions.size(), "valid operand");
    }

    const auto deps = text(R"(module m; parameter real A=2; parameter real B=A*3;
        localparam real C=B+1; parameter P=15; real ids; analog ids=C; endmodule)");
    require(!deps.has_errors(), "dependency HIR");
    const auto d0=H::resolve_parameters(deps), d1=H::resolve_parameters(deps,{{"A",4.0}});
    require(!d0.has_errors() && !d1.has_errors(), "dependency environment");
    close(value(deps,d0,"C"),7,"default dependency");
    close(value(deps,d1,"C"),13,"override propagates");
    require(parameter(deps,"P").type == H::Type::Integer, "untyped parameter inferred");
    require(code(H::resolve_parameters(deps,{{"C",9.0}}).diagnostics,"EVAL_PARAMETER"), "localparam immutable");

    const auto cycles=text(R"(module m; parameter real A=B; parameter real B=A; real ids; analog ids=A; endmodule)");
    require(!cycles.has_errors(), "typed default cycle is an environment question");
    require(code(H::resolve_parameters(cycles).diagnostics,"EVAL_PARAMETER_CYCLE"), "cycle detected");
    const auto broken=H::resolve_parameters(cycles,{{"A",2.0}});
    require(!broken.has_errors(), "override bypasses cyclic default");
    close(value(cycles,broken,"B"),2,"cycle override value");
    const auto untyped_cycle=text(R"(module m; parameter A=B; parameter B=A; real ids; analog ids=1; endmodule)");
    require(code(untyped_cycle.diagnostics,"HIR_PARAMETER_TYPE"), "inference cycle diagnosed");

    const auto ranges=text(R"(module m; parameter real P=1 from [0:2] from [4:5] exclude 1.5;
        real ids; analog ids=P; endmodule)");
    require(!H::resolve_parameters(ranges,{{"P",4.5}}).has_errors(), "from ranges are union");
    require(code(H::resolve_parameters(ranges,{{"P",3.0}}).diagnostics,"EVAL_PARAMETER_RANGE"), "outside union");
    require(code(H::resolve_parameters(ranges,{{"P",1.5}}).diagnostics,"EVAL_PARAMETER_RANGE"), "excluded point");
    const auto spaced=text(R"(module m; parameter real P=1 from ( -inf : +inf ); analog ; endmodule)");
    require(!spaced.has_errors() && !H::resolve_parameters(spaced).has_errors(), "infinity ignores trivia");
    const auto reversed=text(R"(module m; parameter real P=1 from [2:0]; analog ; endmodule)");
    require(code(H::resolve_parameters(reversed).diagnostics,"EVAL_PARAMETER_RANGE"), "reversed range");
    const auto skipped=text(R"(module m; parameter real A=1.0/0.0; real ids; analog ids=A; endmodule)");
    require(code(H::resolve_parameters(skipped).diagnostics,"EVAL_DIV_ZERO"), "invalid default");
    require(!H::resolve_parameters(skipped,{{"A",1.0}}).has_errors(), "override skips invalid default expression");
    const auto conversion=text(R"(module m; parameter integer A=1.5; parameter integer B=-1.5;
        parameter string S="ok"; real ids; analog ids=A+B; endmodule)");
    const auto ce=H::resolve_parameters(conversion);
    require(!conversion.has_errors() && !ce.has_errors(), "scalar conversion HIR");
    close(value(conversion,ce,"A"),2,"round positive tie"); close(value(conversion,ce,"B"),-2,"round negative tie");
    require(code(H::resolve_parameters(conversion,{{"A",std::string("bad")}}).diagnostics,"EVAL_TYPE"), "typed override");
}
void test_rejections() {
    struct Case { std::string body, expected; };
    for (const auto& c : std::vector<Case>{
        {"parameter real P=1; analog P=2;", "HIR_LVALUE"},
        {"electrical n; real ids; analog ids=n+1;", "HIR_TYPE"},
        {"electrical n; analog n;", "HIR_TYPE"},
        {"real ids; analog ids=sqrt(1,2);", "HIR_SIGNATURE"},
        {"real ids; analog ids=max(\"a\",2);", "HIR_TYPE"},
        {"real ids; analog ids=ddt(1);", "HIR_CAPABILITY"},
        {"real ids; analog @(1) ids=1;", "HIR_CAPABILITY"},
        {"electrical n; real ids; analog ids=I(n);", "HIR_CAPABILITY"},
        {"electrical n; analog V(n)<+1;", "HIR_CAPABILITY"},
        {"real ids; analog ids=$abstime;", "HIR_CAPABILITY"},
        {"real ids; analog ids=1.0%2;", "HIR_TYPE"},
        {"integer x; analog x=~8'h0F;", "HIR_CAPABILITY"},
        {"integer x; analog x=8'hFF+8'h01;", "HIR_CAPABILITY"},
        {"parameter MASK=8'h0F; analog ;", "HIR_CAPABILITY"},
        {"real ids; analog ids=$strobe(\"x\");", "HIR_TYPE"},
        {"parameter real P=$temperature; analog ;", "HIR_PARAMETER_CONSTANT"},
        {"real x; parameter real P=x; analog ;", "HIR_PARAMETER_CONSTANT"},
        {"real x=1; analog x=2;", "HIR_CAPABILITY"},
        {"real x[0:1]; analog ;", "HIR_CAPABILITY"},
        {"real ids; analog while(1) ids=1;", "HIR_CAPABILITY"}}) {
        const auto m=text("module m; "+c.body+" endmodule");
        require(m.has_errors() && code(m.diagnostics,c.expected), "rejected semantic case: "+c.body);
        require(H::resolve_parameters(m).has_errors(), "invalid HIR cannot resolve");
    }
    const auto userfn=text(R"(module m; analog function real f; input x; real x; begin f=x; end endfunction
        real ids; analog ids=f(1); endmodule)");
    require(code(userfn.diagnostics,"HIR_CAPABILITY"), "user functions explicitly outside profile");
}
void test_probes() {
    const auto m=text(R"(module m(a,b); inout a,b; electrical a,b; branch(a,b) ab;
        real x,y,z,w; analog begin x=V(a,b); y=V(b,a); z=V(a,a); w=V(ab);
        I(ab)<+2; I(a,b)<+3; I(b,a)<+1; end endmodule)");
    require(!m.has_errors(), "probe HIR");
    std::vector<H::Probe> probes;
    for (const auto& e : m.expressions) if(e.kind==H::ExprKind::ReadPotential) probes.push_back(*e.probe);
    require(probes.size()==4 && probes[0].positive==probes[1].negative &&
        probes[0].negative==probes[1].positive && probes[2].positive==probes[2].negative &&
        probes[3].alias.has_value(), "ordered probe identity");
    H::EvalInputs inputs; inputs.node_voltages={{"a",3},{"b",1}};
    const auto r=H::evaluate_hir(m,H::resolve_parameters(m),inputs);
    require(!r.has_errors(), "probe execution");
    close(H::number(r.observations.at("x")),2,"V(a,b)");
    close(H::number(r.observations.at("y")),-2,"V(b,a)");
    close(H::number(r.observations.at("z")),0,"V(a,a)");
    close(H::number(r.observations.at("w")),2,"V(alias)");
    close(r.branch_flows.at("ab"),4,"contributions add with orientation");
    require(std::count_if(m.statements.begin(),m.statements.end(),[](const auto& s) {
        return s.kind==H::StmtKind::Contribute;
    })==3, "contributions distinct from assignments");
}
void test_macro_origin() {
    const auto m=text("`define BAD sqrt(\"bad\")\nmodule m; real ids; analog ids=`BAD; endmodule\n");
    require(code(m.diagnostics,"HIR_TYPE"), "macro semantic diagnostic");
    require(std::any_of(m.diagnostics.begin(),m.diagnostics.end(),[](const auto& d) {
        return d.primary && !d.related.empty();
    }), "macro provenance in H2 errors");
    std::ostringstream out;
    H::print_typed_hir(out,sample());
    require(out.str().find("ReadPotential")!=std::string::npos &&
        out.str().find("ContributeFlow")!=std::string::npos, "typed dump readable");
}
}
int main() {
    try { test_literals(); test_metadata_and_parameters(); test_rejections(); test_probes(); test_macro_origin(); }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "H2 types, parameters, probes and provenance passed\n";
}
