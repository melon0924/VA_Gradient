#pragma once

#include "vagrad/frontend/syntax/syntax_kind.hpp"
#include "vagrad/frontend/preprocessor/preprocessor.hpp"

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace vagrad {

struct ParseResult;

struct NodeId {
    std::size_t value;
    friend bool operator==(NodeId, NodeId) = default;
};

struct SyntaxElement {
    enum class Kind { Node, Token };
    Kind kind;
    std::size_t index; // Node 时为 NodeId，Token 时为展开 token 索引
};

struct SyntaxNodeData {
    SyntaxKind kind;
    std::vector<SyntaxElement> children;
    std::size_t token_begin;
    std::size_t token_end; // 左闭右开，含归属于节点的空白和注释
};

// 发布后只读的 arena 树。复制 SyntaxTree 共享存储，不复制整棵树。
// NodeId 只在所属 SyntaxTree 内有效。
class SyntaxTree {
public:
    NodeId root() const { return {0}; }
    const SyntaxNodeData& node(NodeId id) const;
    const PreprocessResult& input() const;
    std::size_t node_count() const;
    std::string text(NodeId id) const;

private:
    struct Storage;
    std::shared_ptr<const Storage> storage_;
    explicit SyntaxTree(std::shared_ptr<const Storage> storage);
    friend SyntaxTree build_syntax_tree(const ParseResult& parsed);
};

// 恢复空白和注释，并校验事件平衡、前向父边与每枚 token 的覆盖。
SyntaxTree build_syntax_tree(const ParseResult& parsed);
void print_syntax_tree(std::ostream& out, const SyntaxTree& tree);

} // namespace vagrad
