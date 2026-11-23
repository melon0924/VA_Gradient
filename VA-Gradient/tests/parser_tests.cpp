#include "vagrad/frontend/parser/parser.hpp"
#include "vagrad/frontend/syntax/syntax_tree.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace vagrad;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ParseResult from_text(std::string text) {
    SourceManager sources;
    const auto file = sources.add_virtual("<parser-test.va>", std::move(text));
    return parse(std::make_shared<const PreprocessResult>(preprocess(sources, file)));
}

ParseResult formula(std::string text) {
    return from_text("module m; real ids, a, b, c, d, e; analog begin ids = " +
                     std::move(text) + "; end endmodule\n");
}

std::vector<NodeId> child_nodes(const SyntaxTree& tree, NodeId parent) {
    std::vector<NodeId> result;
    for (const auto& child : tree.node(parent).children) {
        if (child.kind == SyntaxElement::Kind::Node) {
            result.push_back({child.index});
        }
    }
    return result;
}

std::vector<NodeId> find_nodes(const SyntaxTree& tree, SyntaxKind kind) {
    std::vector<NodeId> result;
    std::vector<NodeId> pending{tree.root()};
    while (!pending.empty()) {
        const auto node = pending.back();
        pending.pop_back();
        if (tree.node(node).kind == kind) {
            result.push_back(node);
        }
        const auto children = child_nodes(tree, node);
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return result;
}

std::string op(const SyntaxTree& tree, NodeId parent) {
    for (const auto& child : tree.node(parent).children) {
        if (child.kind == SyntaxElement::Kind::Token) {
            const auto& token = tree.input().tokens[child.index].original;
            if (token.kind == TokenKind::Symbol) {
                return std::string(tree.input().files.at(token.span.file.value)->slice(
                    token.span.begin, token.span.end));
            }
        }
    }
    return {};
}

void check_invariants(const ParseResult& parsed, const SyntaxTree& tree) {
    require(&tree.input() == parsed.input.get(), "tree shares the existing preprocessed snapshot");
    std::ostringstream expected;
    print_preprocessed_source(expected, *parsed.input);
    require(tree.text(tree.root()) == expected.str(), "CST reconstructs all expanded bytes");
    std::vector<std::size_t> expected_events, actual_events;
    for (std::size_t i = 0; i < parsed.input->tokens.size(); ++i) {
        if (!is_trivia(parsed.input->tokens[i].original.kind)) {
            expected_events.push_back(i);
        }
    }
    std::size_t errors = 0;
    for (const auto& event : parsed.events) {
        if (event.kind == ParseEventKind::Token) {
            actual_events.push_back(event.token_index);
        } else if (event.kind == ParseEventKind::Error) {
            require(event.diagnostic_index < parsed.diagnostics.size(), "valid error event");
            ++errors;
        } else if (event.kind == ParseEventKind::StartNode) {
            require(event.node_kind != SyntaxKind::Tombstone, "every marker is completed");
        }
    }
    require(actual_events == expected_events, "one ordered Token event for every significant token");
    require(errors == parsed.diagnostics.size(), "every syntax diagnostic has an event");

    std::vector<SyntaxElement> pending{{SyntaxElement::Kind::Node, tree.root().value}};
    std::size_t next = 0;
    while (!pending.empty()) {
        const auto element = pending.back();
        pending.pop_back();
        if (element.kind == SyntaxElement::Kind::Token) {
            require(element.index == next++, "CST preserves every token, including trivia, exactly once");
        } else {
            const auto& node = tree.node({element.index});
            require(node.token_begin <= node.token_end && node.token_end <= parsed.input->tokens.size(),
                    "valid node range");
            for (const auto child : child_nodes(tree, {element.index})) {
                const auto& data = tree.node(child);
                require(data.token_begin >= node.token_begin && data.token_end <= node.token_end,
                        "parent encloses child token range");
            }
            pending.insert(pending.end(), node.children.rbegin(), node.children.rend());
        }
    }
    require(next == parsed.input->tokens.size(), "all input tokens are in CST");
}

void precedence() {
    const auto parsed = formula("a + b * c");
    require(!parsed.has_errors(), "precedence example parses");
    const auto tree = build_syntax_tree(parsed);
    const auto binaries = find_nodes(tree, SyntaxKind::BinaryExpr);
    require(binaries.size() == 2 && op(tree, binaries[0]) == "+" && op(tree, binaries[1]) == "*",
            "multiply nests under addition");
    const auto operands = child_nodes(tree, binaries[0]);
    require(tree.node(operands[0]).kind == SyntaxKind::NameExpr && operands[1] == binaries[1],
            "multiplication is the right operand of addition");
    require(std::any_of(parsed.events.begin(), parsed.events.end(),
        [](const auto& event) { return event.forward_parent > 0; }), "Pratt uses forward parent events");
    check_invariants(parsed, tree);

    for (const auto& text : {"a - b - c", "a ** b ** c"}) {
        const auto associativity = formula(text);
        const auto syntax = build_syntax_tree(associativity);
        require(!associativity.has_errors(), "left associative expression parses");
        const auto nodes = find_nodes(syntax, SyntaxKind::BinaryExpr);
        require(syntax.node(child_nodes(syntax, nodes[0])[0]).kind == SyntaxKind::BinaryExpr,
                "same-precedence binary operators, including power, associate left");
        check_invariants(associativity, syntax);
    }
    const auto unary = formula("-a ** b");
    const auto unary_tree = build_syntax_tree(unary);
    const auto power = find_nodes(unary_tree, SyntaxKind::BinaryExpr)[0];
    require(unary_tree.node(child_nodes(unary_tree, power)[0]).kind == SyntaxKind::UnaryExpr,
            "unary binds tighter than power");
    check_invariants(unary, unary_tree);

    const auto conditional = formula("a ? b : c ? d : e");
    const auto conditional_tree = build_syntax_tree(conditional);
    require(!conditional.has_errors(), "nested conditional expression");
    const auto conditions = find_nodes(conditional_tree, SyntaxKind::ConditionalExpr);
    require(conditions.size() == 2 && child_nodes(conditional_tree, conditions[0]).back() == conditions[1],
            "conditional operator associates right");
    check_invariants(conditional, conditional_tree);

    const auto calls = formula("f(a, b[3:0]) + -g(c)");
    const auto call_tree = build_syntax_tree(calls);
    require(!calls.has_errors() && find_nodes(call_tree, SyntaxKind::CallExpr).size() == 2 &&
            find_nodes(call_tree, SyntaxKind::IndexExpr).size() == 1, "calls, unary and indexing");
    check_invariants(calls, call_tree);

    const auto replicated = formula("{2{a, b}}");
    const auto replicated_tree = build_syntax_tree(replicated);
    require(!replicated.has_errors() &&
            find_nodes(replicated_tree, SyntaxKind::ReplicationExpr).size() == 1 &&
            find_nodes(replicated_tree, SyntaxKind::ConcatExpr).size() == 1,
            "replicated concatenation");
    check_invariants(replicated, replicated_tree);
}

void extended_syntax() {
    const auto parsed = from_text(
        "nature Voltage; units = \"V\"; access = V; abstol = 1u; endnature\n"
        "nature Current; units = \"A\"; access = I; abstol = 1p; endnature\n"
        "discipline electrical; potential Voltage; flow Current; enddiscipline\n"
        "(* desc = \"extended\" *) module m #(parameter real K = 1, parameter integer N = 3)\n"
        "(input electrical d, input electrical s);\n"
        "parameter real R = 1 from (0:inf) exclude 2;\n"
        "parameter real X = 1 from [0:2];\n"
        "real x, a[0:3]; integer i; branch (d, s) ds;\n"
        "analog function real square;\n"
        "input z; real z; begin square = z * z; end endfunction\n"
        "analog begin : body\n"
        "x = 0;\n"
        "@(initial_step or timer(1n)) x = square(K);\n"
        "for (i = 0; i < N; i = i + 1) begin a[i] = x; end\n"
        "while (x < 2) x = x + 1;\n"
        "repeat (2) x = x - 1;\n"
        "case (i) 0, 1: x = 1; default: x = 0; endcase\n"
        "if (x > 0) if (K > 0) I(ds) <+ x; else I(ds) <+ 0;\n"
        "$strobe(\"x=%g\", x);\n"
        "end : body\n"
        "endmodule\n");
    if (parsed.has_errors()) {
        for (const auto& diagnostic : parsed.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
    }
    require(!parsed.has_errors(), "extended baseline syntax");
    const auto tree = build_syntax_tree(parsed);
    for (const auto kind : {SyntaxKind::NatureDecl, SyntaxKind::DisciplineDecl,
             SyntaxKind::ModuleParameterList, SyntaxKind::AttributeList, SyntaxKind::ParameterRange,
             SyntaxKind::AnalogFunctionDecl, SyntaxKind::EventStmt, SyntaxKind::ForStmt,
             SyntaxKind::WhileStmt, SyntaxKind::RepeatStmt, SyntaxKind::CaseStmt,
             SyntaxKind::ContributionStmt, SyntaxKind::CallExpr}) {
        require(!find_nodes(tree, kind).empty(), "extended node " + std::string(syntax_kind_name(kind)));
    }
    check_invariants(parsed, tree);
}

void recovery() {
    const std::vector<std::string> broken{
        "ids = a + ; ids = b;",
        "ids = a ids = b;",
        "ids = f(a; ids = b;",
        "if (a > ) ids = b; else ids = c;",
        "ids = a ? b : ; ids = c;",
        "ids = a[; ids = b;",
        "ids = (a + b; ids = c;",
        "while () ids = b;",
        "if (a) else ids = b;",
        "case (a) default: ids = ; endcase ids = b;",
        "parameter real X = ; ids = b;",
        "ids = @; ids = b;"
    };
    for (const auto& body : broken) {
        const auto parsed = from_text("module m; real ids, a, b, c; analog begin " + body + " end endmodule");
        require(parsed.has_errors() && !parsed.diagnostics.empty(), "invalid input is diagnosed: " + body);
        const auto tree = build_syntax_tree(parsed);
        check_invariants(parsed, tree);
        require(!find_nodes(tree, SyntaxKind::AssignmentStmt).empty(), "recovery retains assignments");
    }
    const auto missing_end = from_text("module m; analog begin ids = 0;");
    require(missing_end.has_errors(), "missing block/module terminators");
    check_invariants(missing_end, build_syntax_tree(missing_end));

    const auto malformed_module = from_text(
        "module broken; generate; endmodule\nmodule good; analog ids = 1; endmodule\n");
    require(malformed_module.has_errors(), "unsupported syntax is reported");
    const auto recovered = build_syntax_tree(malformed_module);
    require(find_nodes(recovered, SyntaxKind::ModuleDecl).size() == 2, "later module survives recovery");
    check_invariants(malformed_module, recovered);
}

void model() {
    for (const bool soft : {false, true}) {
        ParseResult parsed;
        {
            SourceManager sources;
            PreprocessOptions options;
            if (soft) {
                options.defines = {{"SOFT_THRESHOLD", "1"}, {"TRACE_MOS", "1"}};
            }
            const auto input = sources.load(std::filesystem::path(VAGRAD_TEST_DATA_DIR) / "simple_mos.va");
            parsed = parse(std::make_shared<const PreprocessResult>(preprocess(sources, input, options)));
        }
        require(!parsed.has_errors(), "real example with default or soft/trace configuration");
        const auto tree = build_syntax_tree(parsed);
        require(find_nodes(tree, SyntaxKind::ParameterDecl).size() == 7, "all model parameters");
        require(find_nodes(tree, SyntaxKind::IfStmt).size() == 2 &&
                find_nodes(tree, SyntaxKind::ContributionStmt).size() == 1, "branches and contribution");
        bool header_token = false;
        for (const auto& event : parsed.events) {
            if (event.kind == ParseEventKind::Token) {
                const auto span = parsed.input->tokens[event.token_index].original.span;
                header_token = header_token ||
                    parsed.input->files.at(span.file.value)->path().filename() == "simple_mos_defaults.vams";
            }
        }
        require(header_token, "Parser reuses header macro token sources");
        check_invariants(parsed, tree);
        std::ostringstream events, syntax;
        print_parse_events(events, parsed);
        print_syntax_tree(syntax, tree);
        require(events.str().find("ForwardParent") != std::string::npos &&
                syntax.str().find("BlockComment") != std::string::npos, "inspectable event and CST output");
    }

    const auto invalid_macro = from_text(
        "`define BROKEN(x) ((x) + )\nmodule m; analog ids = `BROKEN(vgs); endmodule\n");
    require(invalid_macro.has_errors() && !invalid_macro.diagnostics[0].related.empty(),
            "syntax error in a macro carries its call/definition context");
    check_invariants(invalid_macro, build_syntax_tree(invalid_macro));
}

void stress_and_builder() {
    for (const auto& text : {"", " \n// trivia only\n", "\xEF\xBB\xBF/* block */\r\n"}) {
        const auto empty = from_text(text);
        require(!empty.has_errors(), "empty or trivia-only input");
        check_invariants(empty, build_syntax_tree(empty));
    }
    std::string chain = "a";
    for (int i = 0; i < 2000; ++i) {
        chain += " + a";
    }
    const auto long_expression = formula(chain);
    require(!long_expression.has_errors(), "long left associative chain avoids recursive left descent");
    check_invariants(long_expression, build_syntax_tree(long_expression));

    const auto too_deep = formula(std::string(150, '(') + "a" + std::string(150, ')'));
    require(too_deep.has_errors() && std::any_of(too_deep.diagnostics.begin(), too_deep.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "PARSE_DEPTH"; }), "nesting limit diagnostic");
    check_invariants(too_deep, build_syntax_tree(too_deep));

    std::string nested_replication = "a";
    for (int i = 0; i < 150; ++i) {
        nested_replication = "{2{" + nested_replication + "}}";
    }
    const auto replication_limit = formula(nested_replication);
    require(replication_limit.has_errors() &&
            std::any_of(replication_limit.diagnostics.begin(), replication_limit.diagnostics.end(),
                [](const auto& diagnostic) { return diagnostic.code == "PARSE_DEPTH"; }),
            "replicated concatenation also respects nesting limit");
    check_invariants(replication_limit, build_syntax_tree(replication_limit));

    // Builder 必须拒绝遗漏有效 token 的错误事件流。
    auto corrupt = formula("a + b");
    const auto event = std::find_if(corrupt.events.begin(), corrupt.events.end(),
        [](const auto& item) { return item.kind == ParseEventKind::Token; });
    corrupt.events.erase(event);
    bool rejected = false;
    try {
        (void)build_syntax_tree(corrupt);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected, "TreeBuilder checks missing terminal events");

    // 释放 ParseResult 后，树仍持有源文件和来源图；复制树也共享这份只读存储。
    const auto retained = [] {
        const auto parsed = formula("a + b");
        return build_syntax_tree(parsed);
    }();
    const auto copy = retained;
    require(copy.text(copy.root()).find("a + b") != std::string::npos, "tree and source snapshot lifetime");
}

} // namespace

int main() {
    try {
        precedence();
        extended_syntax();
        recovery();
        model();
        stress_and_builder();
        std::cout << "parser precedence, events, recovery, CST fidelity and model checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
