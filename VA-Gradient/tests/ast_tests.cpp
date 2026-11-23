#include "vagrad/frontend/ast/ast.hpp"
#include "vagrad/frontend/parser/parser.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace vagrad;
namespace A = vagrad::ast;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string text(const std::optional<A::Token>& token) {
    return token ? std::string(token->text()) : "<missing>";
}
std::string literal(const std::optional<A::Expr>& expr) {
    if (expr) {
        if (const auto value = A::cast<A::LiteralExpr>(expr->without_parens())) {
            return text(value->literal());
        }
    }
    return "<not-literal>";
}
std::string name(const std::optional<A::Expr>& expr) {
    if (expr) {
        if (const auto value = A::cast<A::NameExpr>(expr->without_parens())) {
            return text(value->name());
        }
    }
    return "<not-name>";
}
template<class T>
std::vector<T> find(const A::Node& root) {
    std::vector<T> result;
    std::vector<A::Node> pending{root};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (const auto view = A::cast<T>(current)) result.push_back(*view);
        const auto children = current.children();
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return result;
}
ParseResult parse_text(std::string source) {
    SourceManager sources;
    const auto file = sources.add_virtual("<ast-test.va>", std::move(source));
    return parse(std::make_shared<const PreprocessResult>(preprocess(sources, file)));
}
SyntaxTree valid_tree(std::string source) {
    const auto parsed = parse_text(std::move(source));
    if (parsed.has_errors()) {
        for (const auto& diagnostic : parsed.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
    }
    require(!parsed.has_errors(), "AST fixture parses successfully");
    return build_syntax_tree(parsed);
}

void model() {
    for (const bool soft : {false, true}) {
        SourceManager sources;
        PreprocessOptions options;
        if (soft) options.defines = {{"SOFT_THRESHOLD", "1"}, {"TRACE_MOS", "1"}};
        const auto file = sources.load(std::filesystem::path(VAGRAD_TEST_DATA_DIR) / "simple_mos.va");
        const auto parsed = parse(std::make_shared<const PreprocessResult>(preprocess(sources, file, options)));
        require(!parsed.has_errors(), "existing model configuration");
        const auto tree = build_syntax_tree(parsed);
        const auto before = tree.text(tree.root());
        const auto node_count = tree.node_count();
        const auto root = A::Root::from_tree(tree);
        const auto modules = root.modules();
        require(modules.size() == 1 && text(modules[0].name()) == "simple_mos", "module name");
        const auto module = modules[0];
        const auto ports = module.ports();
        require(ports.size() == 3 && text(ports[0].name()) == "d" && text(ports[2].name()) == "s",
                "ordinary ports");
        require(!ports[0].direction() && !ports[0].data_type(), "implicit port fields remain optional");

        const auto parameters = module.parameters();
        const std::vector<std::string> expected{"G0", "VTH0", "L", "LEAK", "TNOM", "ENABLE", "TRACE_MASK"};
        require(parameters.size() == expected.size(), "all model parameters");
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const auto declarators = parameters[i].declarators();
            require(declarators.size() == 1 && text(declarators[0].name()) == expected[i], "parameter names");
            require(declarators[0].initializer().has_value(), "parameter initializer");
        }
        require(text(parameters[0].data_type()) == "real" && text(parameters[5].data_type()) == "integer",
                "declared parameter types");
        const auto gain = parameters[0].declarators()[0];
        require(literal(gain.initializer()) == "1m", "expanded gain default");
        require(literal(parameters[1].declarators()[0].initializer()) == "0.45", "expanded threshold default");
        const auto attributes = parameters[0].attributes();
        require(attributes.size() == 2 && text(attributes[0].name()) == "desc" &&
                literal(attributes[1].value()) == "\"A/V^2\"", "declaration attributes are direct children");
        require(parameters[1].attributes().empty() && module.attributes().empty(),
                "attributes do not leak to adjacent declarations or ancestors");
        const auto range = gain.parameter_ranges()[0];
        require(text(range.keyword()) == "from" && range.is_interval() &&
                literal(range.lower()) == "0" && literal(range.upper()) == "inf" &&
                range.lower_inclusive() == false && range.upper_inclusive() == false, "open interval");

        const auto gain_literal = A::cast<A::LiteralExpr>(*gain.initializer())->literal().value();
        const auto span = gain_literal.source_span();
        require(tree.input().files.at(span.file.value)->path().filename() == "simple_mos_defaults.vams" &&
                gain_literal.context().kind == ExpansionKind::Macro, "initializer preserves header macro origin");
        require(gain_literal.range().end - gain_literal.range().begin == 2, "expanded token range");
        require(&gain.tree().node(gain.id()) == &tree.node(gain.id()), "view shares actual CST node storage");

        const auto branches = module.branches();
        require(branches.size() == 1 && text(branches[0].names()[0]) == "ds" &&
                branches[0].terminals().size() == 2 && name(branches[0].terminals()[0]) == "d",
                "branch names and terminals");
        const auto analog = module.analog_blocks();
        require(analog.size() == 1 && analog[0].body().has_value(), "analog body");
        const auto block = A::cast<A::BlockStmt>(*analog[0].body()).value();
        require(text(block.label()) == "evaluate_current" && block.statements().size() == (soft ? 8 : 7),
                "block label and direct statement count");
        const auto ifs = find<A::IfStmt>(block);
        require(ifs.size() == 2 && ifs[0].condition() && ifs[0].then_branch() &&
                !ifs[0].else_branch() && ifs[1].else_branch(), "if/else roles");
        const auto contribution = find<A::ContributionStmt>(block)[0];
        require(text(contribution.op()) == "<+" && contribution.lhs() && contribution.rhs(),
                "contribution operands");
        const auto access = A::cast<A::CallExpr>(*contribution.lhs()).value();
        require(name(access.callee()) == "I" && access.arguments().size() == 1 &&
                name(access.arguments()[0]) == "ds", "contribution access call");
        const auto right = A::cast<A::BinaryExpr>(*contribution.rhs()).value();
        require(text(right.op()) == "/" && name(right.lhs()) == "ids" &&
                name(right.rhs()) == "temperature_scale", "contribution right side");

        std::ostringstream dump;
        A::print_ast_view(dump, tree);
        require(dump.str().find("initializer: LiteralExpr value=\"1m\"") != std::string::npos &&
                dump.str().find("ContributionStmt op=\"<+\"") != std::string::npos &&
                dump.str().find("Origin(File[" + std::to_string(span.file.value) + "]") != std::string::npos,
                "inspectable typed output");
        require(tree.text(tree.root()) == before && tree.node_count() == node_count,
                "AST access and export do not modify or rebuild CST");
        require(!root.contains_error_node(), "valid model has no error placeholder");

        std::vector<A::NameExpr> square_arguments;
        for (const auto& value : find<A::NameExpr>(root)) {
            const auto token = value.name();
            if (token && token->context().kind == ExpansionKind::Argument &&
                token->context().macro_name == "SQUARE") {
                square_arguments.push_back(value);
            }
        }
        require(square_arguments.size() == 4, "two square calls each duplicate one argument");
        const auto a = square_arguments[0].name().value();
        const auto b = square_arguments[1].name().value();
        require(a.source_span().file == b.source_span().file &&
                a.source_span().begin == b.source_span().begin && a.source_span().end == b.source_span().end &&
                !(square_arguments[0] == square_arguments[1]) && !(a == b) &&
                a.range().begin != b.range().begin,
                "copied macro arguments share original source but have distinct AST/token identities");
    }
}

void expressions() {
    const auto tree = valid_tree(
        "module e; real ids,a,b,c,d,n; analog begin\n"
        "ids = a + b * c;\n"
        "ids = -a ** b;\n"
        "ids = a ? b : c ? d : a;\n"
        "ids = f(a, b[3:0]) + -g(c);\n"
        "ids = obj.member;\n"
        "ids = a[2 +: 3];\n"
        "ids = {2{a, b}};\n"
        "ids = ((a));\n"
        "n++;\n"
        "end endmodule");
    const auto root = A::Root::from_tree(tree);
    const auto assignments = find<A::AssignmentStmt>(root);
    require(assignments.size() == 8 && name(assignments[0].lhs()) == "ids", "assignment accessors");
    const auto sum = A::cast<A::BinaryExpr>(*assignments[0].rhs()).value();
    require(text(sum.op()) == "+" && name(sum.lhs()) == "a", "binary left role");
    const auto multiply = A::cast<A::BinaryExpr>(*sum.rhs()).value();
    require(text(multiply.op()) == "*" && name(multiply.lhs()) == "b" &&
            name(multiply.rhs()) == "c", "nested binary roles");
    const auto power = A::cast<A::BinaryExpr>(*assignments[1].rhs()).value();
    const auto unary = A::cast<A::UnaryExpr>(*power.lhs()).value();
    require(text(unary.op()) == "-" && name(unary.operand()) == "a", "prefix operand");
    const auto conditional = A::cast<A::ConditionalExpr>(*assignments[2].rhs()).value();
    require(name(conditional.condition()) == "a" && name(conditional.true_value()) == "b" &&
            A::cast<A::ConditionalExpr>(*conditional.false_value()), "conditional roles");

    const auto calls_sum = A::cast<A::BinaryExpr>(*assignments[3].rhs()).value();
    const auto call = A::cast<A::CallExpr>(*calls_sum.lhs()).value();
    require(name(call.callee()) == "f" && call.arguments().size() == 2, "callee and arguments");
    const auto index = A::cast<A::IndexExpr>(call.arguments()[1]).value();
    require(name(index.base()) == "b" && literal(index.index()) == "3" &&
            text(index.separator()) == ":" && literal(index.range_end()) == "0", "part select");
    const auto member = A::cast<A::MemberExpr>(*assignments[4].rhs()).value();
    require(name(member.base()) == "obj" && text(member.member()) == "member", "member roles");
    const auto indexed = A::cast<A::IndexExpr>(*assignments[5].rhs()).value();
    require(literal(indexed.index()) == "2" && text(indexed.separator()) == "+:" &&
            literal(indexed.range_end()) == "3", "indexed part select");
    const auto replication = A::cast<A::ReplicationExpr>(*assignments[6].rhs()).value();
    const auto concat = A::cast<A::ConcatExpr>(*replication.value()).value();
    require(literal(replication.count()) == "2" && concat.elements().size() == 2 &&
            name(concat.elements()[1]) == "b", "replicated concatenation roles");
    require(name(assignments[7].rhs()) == "a", "parentheses can be viewed through without rewriting CST");
    const auto postfix = find<A::PostfixExpr>(root)[0];
    require(text(postfix.op()) == "++" && name(postfix.operand()) == "n", "postfix operand");
}

void declarations_and_statements() {
    const auto tree = valid_tree(
        "nature Voltage; units = \"V\"; access = V; endnature\n"
        "nature Derived : Voltage; units = \"V\"; endnature\n"
        "discipline electrical; potential Voltage; flow Current; enddiscipline\n"
        "(* desc = \"module\" *) module m #(parameter real P = 2, parameter N = 3)\n"
        "(input electrical d, output real s);\n"
        "parameter real A = 1 from [0:2) exclude 0, B = 2;\n"
        "real x[0:3], y; integer i; branch (d,s) ds, ds2;\n"
        "analog function real square; input z; real z; parameter real LOCAL=2;\n"
        "begin square = z*z; end endfunction\n"
        "analog begin : body\n"
        "for (i=0; i<N; i++) x = i;\n"
        "for (;;) break;\n"
        "while (x<2) x=x+1;\n"
        "repeat (2) x=x-1;\n"
        "@(initial_step or timer(1n)) I(d,s) <+ x;\n"
        "case (i) 0,1: x=1; default: begin x=0; end endcase\n"
        "if (x) if (P) x=1; else x=2;\n"
        "end endmodule");
    const auto root = A::Root::from_tree(tree);
    require(root.natures().size() == 2 && text(root.natures()[1].parent_name()) == "Voltage" &&
            text(root.natures()[0].properties()[0].name()) == "units" &&
            literal(root.natures()[0].properties()[0].value()) == "\"V\"", "nature/property views");
    require(text(root.disciplines()[0].name()) == "electrical" &&
            name(root.disciplines()[0].properties()[1].value()) == "Current", "discipline view");
    const auto module = root.modules()[0];
    require(module.attributes().size() == 1 && module.parameters().size() == 3 &&
            text(module.parameters()[0].declarators()[0].name()) == "P" &&
            text(module.parameters()[1].declarators()[0].name()) == "N",
            "header parameters and body parameters, excluding function-local parameters");
    const auto ports = module.ports();
    require(text(ports[0].name()) == "d" && text(ports[0].direction()) == "input" &&
            text(ports[0].data_type()) == "electrical" && text(ports[1].data_type()) == "real",
            "ANSI port type is distinct from name");
    const auto declarations = module.parameters()[2].declarators();
    require(declarations.size() == 2 && text(declarations[1].name()) == "B", "multiple declarators");
    const auto intervals = declarations[0].parameter_ranges();
    require(intervals.size() == 2 && intervals[0].lower_inclusive() == true &&
            intervals[0].upper_inclusive() == false && literal(intervals[1].single_value()) == "0" &&
            !intervals[1].lower() && !intervals[1].upper_inclusive(), "range endpoint and single-value exclusion");
    const auto variables = find<A::VariableDecl>(module);
    const auto array = variables[0].declarators()[0].array_ranges()[0];
    require(literal(array.left()) == "0" && literal(array.right()) == "3", "array range");
    require(module.branches()[0].names().size() == 2, "multiple branch names");
    const auto function = module.functions()[0];
    require(text(function.name()) == "square" && text(function.return_type()) == "real" &&
            function.declarations().size() == 3 && function.statements().size() == 1,
            "analog function fields");

    const auto block = A::cast<A::BlockStmt>(*module.analog_blocks()[0].body()).value();
    const auto fors = find<A::ForStmt>(block);
    require(fors.size() == 2 && fors[0].initializer() && fors[0].condition() &&
            fors[0].step() && fors[0].body(), "for header slots and body");
    require(!fors[1].initializer() && !fors[1].condition() && !fors[1].step() &&
            A::cast<A::JumpStmt>(*fors[1].body()), "empty for slots do not steal body");
    const auto loop = find<A::WhileStmt>(block)[0];
    require(loop.condition() && A::cast<A::AssignmentStmt>(*loop.body()), "while fields");
    const auto repeat = find<A::RepeatStmt>(block)[0];
    require(literal(repeat.count()) == "2" && repeat.body(), "repeat fields");
    const auto event = find<A::EventStmt>(block)[0];
    require(event.events().size() == 2 && name(event.events()[0]) == "initial_step" &&
            A::cast<A::ContributionStmt>(*event.body()), "event list and body");
    const auto cases = find<A::CaseStmt>(block)[0];
    const auto items = cases.items();
    require(name(cases.selector()) == "i" && items.size() == 2 &&
            items[0].labels().size() == 2 && !items[0].is_default() &&
            items[1].is_default() && items[1].labels().empty() &&
            A::cast<A::BlockStmt>(*items[1].body()), "case labels and default body");
    const auto ifs = find<A::IfStmt>(block);
    require(!ifs[0].else_branch() && ifs[1].else_branch(), "dangling else belongs to inner if");
}

void recovery_and_lifetime() {
    const auto parsed = parse_text(
        "module m; parameter real P from [0:1]; parameter real = ;\n"
        "analog begin ids = a + ; if () ; else ; while (a) end endmodule\n");
    require(parsed.has_errors(), "incomplete syntax is diagnosed");
    const auto tree = build_syntax_tree(parsed);
    const auto root = A::Root::from_tree(tree);
    require(root.contains_error_node(), "error placeholders remain visible");
    const auto parameters = root.modules()[0].parameters();
    require(!parameters[0].declarators()[0].initializer() &&
            parameters[0].declarators()[0].parameter_ranges().size() == 1,
            "missing initializer cannot be confused with a range expression");
    require(!parameters[1].declarators()[0].name() &&
            parameters[1].declarators()[0].initializer()->kind() == SyntaxKind::ErrorNode,
            "missing name and error expression remain distinguishable");
    const auto assignment = find<A::AssignmentStmt>(root)[0];
    const auto binary = A::cast<A::BinaryExpr>(*assignment.rhs()).value();
    require(binary.rhs()->kind() == SyntaxKind::ErrorNode && !binary.rhs()->first_token(),
            "missing operand is an empty error expression, not a neighboring expression");
    const auto loop = find<A::WhileStmt>(root)[0];
    require(loop.body() && loop.body()->kind() == SyntaxKind::ErrorNode, "missing loop body");
    std::ostringstream output;
    A::print_ast_view(output, tree);
    require(output.str().find("rhs: ErrorNode missing=true") != std::string::npos, "partial AST dump");

    const auto missing_separator = parse_text("module m; analog begin ids=a ids=b; end endmodule");
    const auto separator_root = A::Root::from_tree(build_syntax_tree(missing_separator));
    require(missing_separator.has_errors() && !separator_root.contains_error_node(),
            "contains_error_node does not replace ParseResult diagnostics");

    const auto empty = A::Root::from_tree(valid_tree("// empty file\n"));
    require(empty.modules().empty() && !empty.first_token(), "trivia-only root");
    require(!A::cast<A::Module>(empty), "checked cast rejects mismatched kind");
    bool mismatch = false;
    try {
        (void)A::NameExpr(empty);
    } catch (const std::invalid_argument&) {
        mismatch = true;
    }
    require(mismatch, "direct typed construction checks kind");

    const auto retained = [] {
        const auto syntax = valid_tree("module retained; parameter real P=2; endmodule");
        return A::Root::from_tree(syntax).modules()[0];
    }();
    require(text(retained.name()) == "retained" && literal(retained.parameters()[0].declarators()[0].initializer()) == "2",
            "typed node retains CST and original source after local objects are destroyed");
    const auto token = [] {
        const auto syntax = valid_tree("module retained_token; endmodule");
        return A::Root::from_tree(syntax).modules()[0].name().value();
    }();
    require(token.text() == "retained_token", "token handle lifetime");

    const auto original = A::Root::from_tree(valid_tree("module m; endmodule"));
    const auto copy = original;
    const auto other = A::Root::from_tree(valid_tree("module m; endmodule"));
    require(original == copy && !(original == other), "node identity includes tree storage, not just NodeId");

    std::string chain = "a";
    for (int i = 0; i < 2000; ++i) chain += " + a";
    const auto large = A::Root::from_tree(valid_tree("module m; analog ids=" + chain + "; endmodule"));
    require(find<A::BinaryExpr>(large).size() == 2000 && !large.contains_error_node(),
            "view traversal does not recursively rebuild a deep expression");
}

} // namespace

int main() {
    try {
        model();
        expressions();
        declarations_and_statements();
        recovery_and_lifetime();
        std::cout << "AST View fields, scopes, source mapping, error tolerance and storage sharing passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
