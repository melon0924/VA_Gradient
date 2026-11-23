#include "vagrad/frontend/syntax/syntax_tree.hpp"
#include "vagrad/frontend/parser/parser.hpp"
#include "vagrad/frontend/lexer/lexer.hpp"

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace vagrad {

struct SyntaxTree::Storage {
    std::shared_ptr<const PreprocessResult> input;
    std::vector<SyntaxNodeData> nodes;
};

SyntaxTree::SyntaxTree(std::shared_ptr<const Storage> storage) : storage_(std::move(storage)) {}
const SyntaxNodeData& SyntaxTree::node(NodeId id) const { return storage_->nodes.at(id.value); }
const PreprocessResult& SyntaxTree::input() const { return *storage_->input; }
std::size_t SyntaxTree::node_count() const { return storage_->nodes.size(); }

std::string SyntaxTree::text(NodeId id) const {
    const auto& data = node(id);
    std::string result;
    for (auto i = data.token_begin; i < data.token_end; ++i) {
        const auto span = input().tokens.at(i).original.span;
        result += input().files.at(span.file.value)->slice(span.begin, span.end);
    }
    return result;
}

SyntaxTree build_syntax_tree(const ParseResult& parsed) {
    if (!parsed.input || parsed.input->tokens.empty()) {
        throw std::invalid_argument("TreeBuilder requires an input token stream");
    }
    auto storage = std::make_shared<SyntaxTree::Storage>();
    storage->input = parsed.input;
    std::vector<NodeId> stack;
    std::vector<bool> opened(parsed.events.size(), false);
    std::size_t next_token = 0;
    const auto fail = [] { throw std::logic_error("invalid parse event stream"); };
    const auto& tokens = parsed.input->tokens;

    for (std::size_t i = 0; i < parsed.events.size(); ++i) {
        const auto& event = parsed.events[i];
        switch (event.kind) {
        case ParseEventKind::StartNode: {
            if (opened[i] || event.node_kind == SyntaxKind::Tombstone) {
                break;
            }
            // 收集“左表达式 -> 后出现的外层表达式”链，反过来打开父节点。
            std::vector<SyntaxKind> parents;
            auto current = i;
            while (true) {
                const auto& start = parsed.events[current];
                if (start.kind != ParseEventKind::StartNode || opened[current] ||
                    start.node_kind == SyntaxKind::Tombstone) {
                    fail();
                }
                opened[current] = true;
                parents.push_back(start.node_kind);
                if (start.forward_parent == 0) {
                    break;
                }
                if (start.forward_parent >= parsed.events.size() - current) {
                    fail();
                }
                current += start.forward_parent;
            }
            for (auto parent = parents.rbegin(); parent != parents.rend(); ++parent) {
                if (stack.empty() && !storage->nodes.empty()) {
                    fail(); // 只能有一棵根树
                }
                const NodeId id{storage->nodes.size()};
                storage->nodes.push_back({*parent, {}, next_token, next_token});
                if (!stack.empty()) {
                    storage->nodes[stack.back().value].children.push_back(
                        {SyntaxElement::Kind::Node, id.value});
                }
                stack.push_back(id);
            }
            break;
        }
        case ParseEventKind::Token:
            if (stack.empty() || event.token_index < next_token || event.token_index >= tokens.size()) {
                fail();
            }
            // Parser 只报告有效记号；中间的 trivia 在此按原顺序补回。
            while (next_token <= event.token_index) {
                if (next_token < event.token_index && !is_trivia(tokens[next_token].original.kind)) {
                    fail(); // 有效 token 不允许被 Parser 静默丢弃
                }
                storage->nodes[stack.back().value].children.push_back(
                    {SyntaxElement::Kind::Token, next_token++});
            }
            break;
        case ParseEventKind::FinishNode:
            if (stack.empty()) {
                fail();
            }
            storage->nodes[stack.back().value].token_end = next_token;
            stack.pop_back();
            break;
        case ParseEventKind::Error:
            if (event.diagnostic_index >= parsed.diagnostics.size()) {
                fail();
            }
            break; // 诊断不产生虚构的源码 token
        }
    }
    if (!stack.empty() || storage->nodes.empty() || storage->nodes[0].kind != SyntaxKind::Root ||
        next_token != tokens.size()) {
        fail();
    }
    return SyntaxTree(std::move(storage));
}

void print_syntax_tree(std::ostream& out, const SyntaxTree& tree) {
    print_source_map(out, tree.input());
    struct Work { SyntaxElement element; std::size_t depth; };
    std::vector<Work> pending{{{SyntaxElement::Kind::Node, tree.root().value}, 0}};
    const auto& tokens = tree.input().tokens;
    while (!pending.empty()) {
        const auto [element, depth] = pending.back();
        pending.pop_back();
        out << std::string(depth * 2, ' ');
        if (element.kind == SyntaxElement::Kind::Node) {
            const auto& node = tree.node({element.index});
            const auto begin = node.token_begin < tokens.size() ?
                tokens[node.token_begin].begin : tokens.back().end;
            const auto end = node.token_end > node.token_begin ?
                tokens[node.token_end - 1].end : begin;
            out << syntax_kind_name(node.kind) << " Expanded[" << begin << ',' << end << ")\n";
            for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
                pending.push_back({*child, depth + 1});
            }
        } else {
            const auto& token = tokens.at(element.index);
            out << "Token[" << element.index << "] Expanded[" << token.begin << ',' << token.end
                << ") File[" << token.original.span.file.value << "] Context[" << token.context << "] ";
            print_token(out, *tree.input().files.at(token.original.span.file.value), token.original);
            out << '\n';
        }
    }
}

} // namespace vagrad
