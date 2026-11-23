#pragma once

#include "vagrad/frontend/syntax/syntax_tree.hpp"

#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace vagrad::ast {

struct ExpandedRange { std::size_t begin, end; };
class Attribute;
class Module;
class NatureDecl;
class DisciplineDecl;
class Declarator;
class ParameterDecl;
class Port;
class BranchDecl;
class AnalogBlock;
class AnalogFunction;
class ParameterRange;
class ArrayRange;
class CaseItem;
class PropertyDecl;

// 叶子也共享 CST 的存储，因此 Token 句柄离开原树变量的作用域后仍可使用。
class Token {
public:
    Token(SyntaxTree tree, std::size_t index);
    const SyntaxTree& tree() const { return tree_; }
    std::size_t index() const { return index_; }
    TokenKind kind() const;
    std::string_view text() const;
    SourceSpan source_span() const;
    ExpandedRange range() const;
    std::size_t context_index() const;
    const ExpansionContext& context() const;
    friend bool operator==(const Token& a, const Token& b);
private:
    SyntaxTree tree_;
    std::size_t index_;
};

// Node = 一份很轻的 CST 句柄，不分配第二棵 AST，不重新识别源码。
class Node {
public:
    Node(SyntaxTree tree, NodeId id);
    const SyntaxTree& tree() const { return tree_; }
    NodeId id() const { return id_; }
    SyntaxKind kind() const;
    ExpandedRange range() const;
    std::string text() const;
    std::vector<Node> children() const;  // 直接子节点
    std::vector<Token> tokens() const;   // 直接、非 trivia token，非递归
    std::optional<Token> first_token() const; // 子树内首枚有效 token，跳过 EOF
    std::vector<Attribute> attributes() const;
    bool contains_error_node() const;
    friend bool operator==(const Node& a, const Node& b);
private:
    SyntaxTree tree_;
    NodeId id_;
};

// cast 失败返回 nullopt，直接用错误种类构造类型化节点则抛 invalid_argument。
template<class T>
std::optional<T> cast(const Node& node) {
    if (!T::can_cast(node.kind())) {
        return std::nullopt;
    }
    return T(node);
}

template<SyntaxKind Expected, class Base = Node>
class TypedNode : public Base {
public:
    explicit TypedNode(Node node) : Base(std::move(node)) {
        if (!can_cast(this->kind())) {
            throw std::invalid_argument("AST View kind mismatch");
        }
    }
    static bool can_cast(SyntaxKind kind) { return kind == Expected; }
};

class Expr : public Node {
public:
    explicit Expr(Node node);
    static bool can_cast(SyntaxKind kind);
    std::vector<Expr> operands() const;
    Expr without_parens() const;
};

class Statement : public Node {
public:
    explicit Statement(Node node);
    static bool can_cast(SyntaxKind kind);
};

class Declaration : public Node {
public:
    explicit Declaration(Node node);
    static bool can_cast(SyntaxKind kind);
    std::optional<Token> keyword() const;
    std::optional<Token> data_type() const; // 只取语法中的类型文字，不做符号绑定
    std::vector<Declarator> declarators() const;
};

class NameExpr : public TypedNode<SyntaxKind::NameExpr, Expr> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> name() const;
};
class LiteralExpr : public TypedNode<SyntaxKind::LiteralExpr, Expr> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> literal() const;
};
class UnaryExpr : public TypedNode<SyntaxKind::UnaryExpr, Expr> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> op() const;
    std::optional<Expr> operand() const;
};
class BinaryExpr : public TypedNode<SyntaxKind::BinaryExpr, Expr> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> op() const;
    std::optional<Expr> lhs() const;
    std::optional<Expr> rhs() const;
};
class ConditionalExpr : public TypedNode<SyntaxKind::ConditionalExpr, Expr> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> condition() const;
    std::optional<Expr> true_value() const;
    std::optional<Expr> false_value() const;
};
class ParenExpr : public TypedNode<SyntaxKind::ParenExpr, Expr> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> inner() const;
};
class CallExpr : public TypedNode<SyntaxKind::CallExpr, Expr> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> callee() const;
    std::vector<Expr> arguments() const;
};
class ArgumentList : public TypedNode<SyntaxKind::ArgumentList> {
public:
    using TypedNode::TypedNode;
    std::vector<Expr> arguments() const;
};
class IndexExpr : public TypedNode<SyntaxKind::IndexExpr, Expr> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> base() const;
    std::optional<Expr> index() const;
    std::optional<Token> separator() const; // :、+: 或 -:
    std::optional<Expr> range_end() const;
};
class MemberExpr : public TypedNode<SyntaxKind::MemberExpr, Expr> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> base() const;
    std::optional<Token> member() const;
};
class PostfixExpr : public TypedNode<SyntaxKind::PostfixExpr, Expr> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> operand() const;
    std::optional<Token> op() const;
};
class ConcatExpr : public TypedNode<SyntaxKind::ConcatExpr, Expr> {
public:
    using TypedNode::TypedNode;
    std::vector<Expr> elements() const;
};
class ReplicationExpr : public TypedNode<SyntaxKind::ReplicationExpr, Expr> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> count() const;
    std::optional<Expr> value() const;
};

class Attribute : public TypedNode<SyntaxKind::Attribute> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> name() const;
    std::optional<Expr> value() const;
};
class AttributeList : public TypedNode<SyntaxKind::AttributeList> {
public:
    using TypedNode::TypedNode;
    std::vector<Attribute> attributes() const;
};
class ArrayRange : public TypedNode<SyntaxKind::ArrayRange> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> left() const;
    std::optional<Expr> right() const;
};
class ParameterRange : public TypedNode<SyntaxKind::ParameterRange> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> keyword() const; // from / exclude
    bool is_interval() const;
    std::optional<Expr> lower() const;
    std::optional<Expr> upper() const;
    std::optional<bool> lower_inclusive() const;
    std::optional<bool> upper_inclusive() const;
    std::optional<Expr> single_value() const;
};
class Declarator : public TypedNode<SyntaxKind::Declarator> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> name() const;
    std::optional<Expr> initializer() const;
    std::vector<ArrayRange> array_ranges() const;
    std::vector<ParameterRange> parameter_ranges() const;
};
class ParameterDecl : public TypedNode<SyntaxKind::ParameterDecl, Declaration> {
public:
    using TypedNode::TypedNode;
};
class VariableDecl : public TypedNode<SyntaxKind::VariableDecl, Declaration> {
public:
    using TypedNode::TypedNode;
};
class NetDecl : public TypedNode<SyntaxKind::NetDecl, Declaration> {
public:
    using TypedNode::TypedNode;
};
class PortDecl : public TypedNode<SyntaxKind::PortDecl, Declaration> {
public:
    using TypedNode::TypedNode;
};
class Port : public TypedNode<SyntaxKind::Port> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> name() const;
    std::optional<Token> direction() const;
    std::optional<Token> data_type() const;
    std::optional<Expr> default_value() const;
    std::vector<ArrayRange> array_ranges() const;
};
class BranchDecl : public TypedNode<SyntaxKind::BranchDecl> {
public:
    using TypedNode::TypedNode;
    std::vector<Token> names() const;
    std::vector<Expr> terminals() const;
};
class AnalogFunction : public TypedNode<SyntaxKind::AnalogFunctionDecl> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> name() const;
    std::optional<Token> return_type() const;
    std::vector<Port> ports() const;
    std::vector<Declaration> declarations() const;
    std::vector<Statement> statements() const;
};

class Assignment : public Statement {
public:
    explicit Assignment(Node node);
    static bool can_cast(SyntaxKind kind);
    std::optional<Token> op() const;
    std::optional<Expr> lhs() const;
    std::optional<Expr> rhs() const;
};
class AssignmentStmt : public TypedNode<SyntaxKind::AssignmentStmt, Assignment> {
public:
    using TypedNode::TypedNode;
};
class ContributionStmt : public TypedNode<SyntaxKind::ContributionStmt, Assignment> {
public:
    using TypedNode::TypedNode;
};
class ExprStmt : public TypedNode<SyntaxKind::ExprStmt, Statement> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> expression() const;
};
class EmptyStmt : public TypedNode<SyntaxKind::EmptyStmt, Statement> {
public:
    using TypedNode::TypedNode;
};
class BlockStmt : public TypedNode<SyntaxKind::BlockStmt, Statement> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> label() const;
    std::vector<Node> items() const;
    std::vector<Statement> statements() const;
    std::vector<Declaration> declarations() const;
};
class IfStmt : public TypedNode<SyntaxKind::IfStmt, Statement> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> condition() const;
    std::optional<Statement> then_branch() const;
    std::optional<Statement> else_branch() const;
};
class ForStmt : public TypedNode<SyntaxKind::ForStmt, Statement> {
public:
    using TypedNode::TypedNode;
    std::optional<Statement> initializer() const;
    std::optional<Expr> condition() const;
    std::optional<Statement> step() const;
    std::optional<Statement> body() const;
};
class WhileStmt : public TypedNode<SyntaxKind::WhileStmt, Statement> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> condition() const;
    std::optional<Statement> body() const;
};
class RepeatStmt : public TypedNode<SyntaxKind::RepeatStmt, Statement> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> count() const;
    std::optional<Statement> body() const;
};
class EventList : public TypedNode<SyntaxKind::EventList> {
public:
    using TypedNode::TypedNode;
    std::vector<Expr> events() const;
};
class EventStmt : public TypedNode<SyntaxKind::EventStmt, Statement> {
public:
    using TypedNode::TypedNode;
    std::vector<Expr> events() const;
    std::optional<Statement> body() const;
};
class CaseItem : public TypedNode<SyntaxKind::CaseItem> {
public:
    using TypedNode::TypedNode;
    bool is_default() const;
    std::vector<Expr> labels() const;
    std::optional<Statement> body() const;
};
class CaseStmt : public TypedNode<SyntaxKind::CaseStmt, Statement> {
public:
    using TypedNode::TypedNode;
    std::optional<Expr> selector() const;
    std::vector<CaseItem> items() const;
};
class JumpStmt : public TypedNode<SyntaxKind::JumpStmt, Statement> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> keyword() const;
    std::optional<Expr> value() const;
};
class AnalogBlock : public TypedNode<SyntaxKind::AnalogBlock> {
public:
    using TypedNode::TypedNode;
    bool is_initial() const;
    std::optional<Statement> body() const;
};
class PropertyDecl : public TypedNode<SyntaxKind::PropertyDecl> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> name() const;
    std::optional<Expr> value() const;
};
class NatureDecl : public TypedNode<SyntaxKind::NatureDecl> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> name() const;
    std::optional<Token> parent_name() const;
    std::vector<PropertyDecl> properties() const;
};
class DisciplineDecl : public TypedNode<SyntaxKind::DisciplineDecl> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> name() const;
    std::vector<PropertyDecl> properties() const;
};
class Module : public TypedNode<SyntaxKind::ModuleDecl> {
public:
    using TypedNode::TypedNode;
    std::optional<Token> name() const;
    std::vector<Port> ports() const;
    std::vector<ParameterDecl> parameters() const; // 包括 #(...) 中的参数
    std::vector<Declaration> declarations() const; // 模块体内的直接声明
    std::vector<BranchDecl> branches() const;
    std::vector<AnalogBlock> analog_blocks() const;
    std::vector<AnalogFunction> functions() const;
    std::vector<Node> items() const;
};
class Root : public TypedNode<SyntaxKind::Root> {
public:
    using TypedNode::TypedNode;
    static Root from_tree(const SyntaxTree& tree);
    std::vector<Module> modules() const;
    std::vector<NatureDecl> natures() const;
    std::vector<DisciplineDecl> disciplines() const;
};

// 供调试阅读的类型化视图；继续采用现有 TXT 导出方式。
void print_ast_view(std::ostream& out, const SyntaxTree& tree);

} // namespace vagrad::ast
