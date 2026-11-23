#include "vagrad/frontend/ast/ast.hpp"

#include <algorithm>
#include <initializer_list>
#include <limits>

namespace vagrad::ast {
namespace {

constexpr auto unlimited = std::numeric_limits<std::size_t>::max();

bool is_name(TokenKind kind) {
    return kind == TokenKind::Identifier || kind == TokenKind::EscapedIdentifier ||
           kind == TokenKind::SystemIdentifier;
}
bool in(std::string_view text, std::initializer_list<std::string_view> choices) {
    return std::find(choices.begin(), choices.end(), text) != choices.end();
}
bool is_type(std::string_view text) {
    return in(text, {"real", "integer", "string", "genvar", "time", "realtime", "reg", "wire", "wreal"});
}
const std::vector<SyntaxElement>& elements(const Node& node) {
    return node.tree().node(node.id()).children;
}

// 所有字段访问都在“当前节点的直接 children”中寻找，不钻进嵌套表达式抢字段。
template<class T>
std::vector<T> all(const Node& node, std::size_t begin = 0, std::size_t end = unlimited) {
    std::vector<T> result;
    const auto& children = elements(node);
    for (auto i = begin; i < std::min(end, children.size()); ++i) {
        if (children[i].kind == SyntaxElement::Kind::Node) {
            if (auto value = cast<T>(Node(node.tree(), {children[i].index}))) {
                result.push_back(*value);
            }
        }
    }
    return result;
}
template<class T>
std::optional<T> first(const Node& node, std::size_t begin = 0, std::size_t end = unlimited) {
    const auto& children = elements(node);
    for (auto i = begin; i < std::min(end, children.size()); ++i) {
        if (children[i].kind == SyntaxElement::Kind::Node) {
            if (auto value = cast<T>(Node(node.tree(), {children[i].index}))) {
                return value;
            }
        }
    }
    return std::nullopt;
}
template<class Predicate>
std::optional<Token> direct_token(const Node& node, Predicate accepts,
                                  std::size_t begin = 0, std::size_t end = unlimited) {
    const auto& children = elements(node);
    for (auto i = begin; i < std::min(end, children.size()); ++i) {
        if (children[i].kind == SyntaxElement::Kind::Token) {
            const Token token(node.tree(), children[i].index);
            if (!is_trivia(token.kind()) && accepts(token)) {
                return token;
            }
        }
    }
    return std::nullopt;
}
std::optional<std::size_t> position(const Node& node,
                                  std::initializer_list<std::string_view> choices,
                                  std::size_t occurrence = 0, std::size_t begin = 0) {
    const auto& children = elements(node);
    for (auto i = begin; i < children.size(); ++i) {
        if (children[i].kind == SyntaxElement::Kind::Token) {
            const Token token(node.tree(), children[i].index);
            if (!is_trivia(token.kind()) && in(token.text(), choices)) {
                if (occurrence == 0) {
                    return i;
                }
                --occurrence;
            }
        }
    }
    return std::nullopt;
}
std::optional<Token> matching(const Node& node,
                              std::initializer_list<std::string_view> choices) {
    return direct_token(node, [choices](const Token& token) { return in(token.text(), choices); });
}
std::optional<Token> identifier(const Node& node, std::size_t begin = 0,
                                std::size_t end = unlimited) {
    return direct_token(node, [](const Token& token) { return is_name(token.kind()); }, begin, end);
}
template<class T>
std::optional<T> after(const Node& node, std::initializer_list<std::string_view> choices) {
    const auto at = position(node, choices);
    return at ? first<T>(node, *at + 1) : std::nullopt;
}
template<class T>
std::optional<T> before(const Node& node, std::initializer_list<std::string_view> choices) {
    const auto at = position(node, choices);
    return at ? first<T>(node, 0, *at) : std::nullopt;
}
template<class T>
std::optional<T> between(const Node& node, std::string_view left, std::string_view right) {
    const auto start = position(node, {left});
    if (!start) {
        return std::nullopt;
    }
    const auto finish = position(node, {right}, 0, *start + 1);
    return finish ? first<T>(node, *start + 1, *finish) : std::nullopt;
}
std::optional<Token> first_direct(const Node& node) {
    return direct_token(node, [](const Token&) { return true; });
}
std::optional<Expr> operand_before(const Node& node, const std::optional<Token>& op) {
    if (!op) {
        return std::nullopt;
    }
    return before<Expr>(node, {op->text()});
}
std::optional<Expr> operand_after(const Node& node, const std::optional<Token>& op) {
    if (!op) {
        return std::nullopt;
    }
    return after<Expr>(node, {op->text()});
}
std::vector<Port> ports_of(const Node& node) {
    for (const auto& child : node.children()) {
        if (child.kind() == SyntaxKind::ModulePortList) {
            return all<Port>(child);
        }
    }
    return {};
}

} // namespace

Token::Token(SyntaxTree tree, std::size_t index) : tree_(std::move(tree)), index_(index) {
    (void)tree_.input().tokens.at(index_);
}
TokenKind Token::kind() const { return tree_.input().tokens[index_].original.kind; }
std::string_view Token::text() const {
    const auto span = source_span();
    return tree_.input().files.at(span.file.value)->slice(span.begin, span.end);
}
SourceSpan Token::source_span() const { return tree_.input().tokens[index_].original.span; }
ExpandedRange Token::range() const {
    const auto& token = tree_.input().tokens[index_];
    return {token.begin, token.end};
}
std::size_t Token::context_index() const { return tree_.input().tokens[index_].context; }
const ExpansionContext& Token::context() const { return tree_.input().contexts.at(context_index()); }
bool operator==(const Token& a, const Token& b) {
    return &a.tree_.input().tokens[a.index_] == &b.tree_.input().tokens[b.index_];
}

Node::Node(SyntaxTree tree, NodeId id) : tree_(std::move(tree)), id_(id) {
    (void)tree_.node(id_);
}
SyntaxKind Node::kind() const { return tree_.node(id_).kind; }
ExpandedRange Node::range() const {
    const auto& node = tree_.node(id_);
    const auto& tokens = tree_.input().tokens;
    const auto begin = node.token_begin < tokens.size() ? tokens[node.token_begin].begin : tokens.back().end;
    const auto end = node.token_end > node.token_begin ? tokens[node.token_end - 1].end : begin;
    return {begin, end};
}
std::string Node::text() const { return tree_.text(id_); }
std::vector<Node> Node::children() const {
    std::vector<Node> result;
    for (const auto& child : elements(*this)) {
        if (child.kind == SyntaxElement::Kind::Node) {
            result.emplace_back(tree_, NodeId{child.index});
        }
    }
    return result;
}
std::vector<Token> Node::tokens() const {
    std::vector<Token> result;
    for (const auto& child : elements(*this)) {
        if (child.kind == SyntaxElement::Kind::Token) {
            Token token(tree_, child.index);
            if (!is_trivia(token.kind())) {
                result.push_back(token);
            }
        }
    }
    return result;
}
std::optional<Token> Node::first_token() const {
    const auto& node = tree_.node(id_);
    for (auto i = node.token_begin; i < node.token_end; ++i) {
        Token token(tree_, i);
        if (!is_trivia(token.kind()) && token.kind() != TokenKind::EndOfFile) {
            return token;
        }
    }
    return std::nullopt;
}
std::vector<Attribute> Node::attributes() const {
    std::vector<Attribute> result;
    for (const auto& list : all<AttributeList>(*this)) {
        const auto attributes = list.attributes();
        result.insert(result.end(), attributes.begin(), attributes.end());
    }
    return result;
}
bool Node::contains_error_node() const {
    std::vector<NodeId> pending{id_};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        const auto& data = tree_.node(id);
        if (data.kind == SyntaxKind::ErrorNode) {
            return true;
        }
        for (const auto& child : data.children) {
            if (child.kind == SyntaxElement::Kind::Node) {
                pending.push_back({child.index});
            }
        }
    }
    return false;
}
bool operator==(const Node& a, const Node& b) {
    return &a.tree_.node(a.id_) == &b.tree_.node(b.id_);
}

bool Expr::can_cast(SyntaxKind kind) {
    switch (kind) {
    case SyntaxKind::NameExpr: case SyntaxKind::LiteralExpr: case SyntaxKind::UnaryExpr:
    case SyntaxKind::BinaryExpr: case SyntaxKind::ConditionalExpr: case SyntaxKind::ParenExpr:
    case SyntaxKind::CallExpr: case SyntaxKind::IndexExpr: case SyntaxKind::MemberExpr:
    case SyntaxKind::PostfixExpr: case SyntaxKind::ConcatExpr: case SyntaxKind::ReplicationExpr:
    case SyntaxKind::ErrorNode: return true;
    default: return false;
    }
}
Expr::Expr(Node node) : Node(std::move(node)) {
    if (!can_cast(kind())) throw std::invalid_argument("AST View requires an expression");
}
std::vector<Expr> Expr::operands() const { return all<Expr>(*this); }
Expr Expr::without_parens() const {
    Expr current = *this;
    while (const auto parens = cast<ParenExpr>(current)) {
        const auto child = parens->inner();
        if (!child) break;
        current = *child;
    }
    return current;
}
bool Statement::can_cast(SyntaxKind kind) {
    switch (kind) {
    case SyntaxKind::BlockStmt: case SyntaxKind::IfStmt: case SyntaxKind::CaseStmt:
    case SyntaxKind::ForStmt: case SyntaxKind::WhileStmt: case SyntaxKind::RepeatStmt:
    case SyntaxKind::EventStmt: case SyntaxKind::AssignmentStmt: case SyntaxKind::ContributionStmt:
    case SyntaxKind::ExprStmt: case SyntaxKind::EmptyStmt: case SyntaxKind::JumpStmt:
    case SyntaxKind::ErrorNode: return true;
    default: return false;
    }
}
Statement::Statement(Node node) : Node(std::move(node)) {
    if (!can_cast(kind())) throw std::invalid_argument("AST View requires a statement");
}
bool Declaration::can_cast(SyntaxKind kind) {
    return kind == SyntaxKind::ParameterDecl || kind == SyntaxKind::VariableDecl ||
           kind == SyntaxKind::NetDecl || kind == SyntaxKind::PortDecl;
}
Declaration::Declaration(Node node) : Node(std::move(node)) {
    if (!can_cast(kind())) throw std::invalid_argument("AST View requires a declaration");
}
std::optional<Token> Declaration::keyword() const { return first_direct(*this); }
std::optional<Token> Declaration::data_type() const {
    return direct_token(*this, [](const Token& token) {
        return is_type(token.text()) || is_name(token.kind());
    });
}
std::vector<Declarator> Declaration::declarators() const { return all<Declarator>(*this); }

std::optional<Token> NameExpr::name() const { return identifier(*this); }
std::optional<Token> LiteralExpr::literal() const { return first_direct(*this); }
std::optional<Token> UnaryExpr::op() const {
    return matching(*this, {"+", "-", "!", "~", "&", "~&", "|", "~|", "^", "~^", "^~", "++", "--"});
}
std::optional<Expr> UnaryExpr::operand() const { return operand_after(*this, op()); }
std::optional<Token> BinaryExpr::op() const {
    return matching(*this, {"**", "*", "/", "%", "+", "-", "<<", ">>", "<<<", ">>>",
                            "<", "<=", ">", ">=", "==", "!=", "===", "!==", "&",
                            "^", "^~", "~^", "|", "&&", "||"});
}
std::optional<Expr> BinaryExpr::lhs() const { return operand_before(*this, op()); }
std::optional<Expr> BinaryExpr::rhs() const { return operand_after(*this, op()); }
std::optional<Expr> ConditionalExpr::condition() const { return before<Expr>(*this, {"?"}); }
std::optional<Expr> ConditionalExpr::true_value() const { return between<Expr>(*this, "?", ":"); }
std::optional<Expr> ConditionalExpr::false_value() const { return after<Expr>(*this, {":"}); }
std::optional<Expr> ParenExpr::inner() const { return first<Expr>(*this); }
std::optional<Expr> CallExpr::callee() const { return first<Expr>(*this); }
std::vector<Expr> CallExpr::arguments() const {
    const auto list = first<ArgumentList>(*this);
    return list ? list->arguments() : std::vector<Expr>{};
}
std::vector<Expr> ArgumentList::arguments() const { return all<Expr>(*this); }
std::optional<Expr> IndexExpr::base() const { return before<Expr>(*this, {"["}); }
std::optional<Expr> IndexExpr::index() const { return after<Expr>(*this, {"["}); }
std::optional<Token> IndexExpr::separator() const { return matching(*this, {":", "+:", "-:"}); }
std::optional<Expr> IndexExpr::range_end() const { return operand_after(*this, separator()); }
std::optional<Expr> MemberExpr::base() const { return before<Expr>(*this, {"."}); }
std::optional<Token> MemberExpr::member() const {
    const auto dot = position(*this, {"."});
    return dot ? identifier(*this, *dot + 1) : std::nullopt;
}
std::optional<Expr> PostfixExpr::operand() const { return operand_before(*this, op()); }
std::optional<Token> PostfixExpr::op() const { return matching(*this, {"++", "--"}); }
std::vector<Expr> ConcatExpr::elements() const { return all<Expr>(*this); }
std::optional<Expr> ReplicationExpr::count() const { return first<Expr>(*this); }
std::optional<Expr> ReplicationExpr::value() const {
    const auto values = all<Expr>(*this);
    return values.size() > 1 ? std::optional<Expr>{values[1]} : std::nullopt;
}

std::optional<Token> Attribute::name() const { return identifier(*this); }
std::optional<Expr> Attribute::value() const { return after<Expr>(*this, {"="}); }
std::vector<Attribute> AttributeList::attributes() const { return all<Attribute>(*this); }
std::optional<Expr> ArrayRange::left() const { return after<Expr>(*this, {"["}); }
std::optional<Expr> ArrayRange::right() const { return after<Expr>(*this, {":"}); }
std::optional<Token> ParameterRange::keyword() const { return matching(*this, {"from", "exclude"}); }
bool ParameterRange::is_interval() const { return position(*this, {"(", "["}).has_value(); }
std::optional<Expr> ParameterRange::lower() const { return after<Expr>(*this, {"(", "["}); }
std::optional<Expr> ParameterRange::upper() const { return after<Expr>(*this, {":"}); }
std::optional<bool> ParameterRange::lower_inclusive() const {
    const auto open = matching(*this, {"(", "["});
    return open ? std::optional<bool>{open->text() == "["} : std::nullopt;
}
std::optional<bool> ParameterRange::upper_inclusive() const {
    const auto close = matching(*this, {")", "]"});
    return close ? std::optional<bool>{close->text() == "]"} : std::nullopt;
}
std::optional<Expr> ParameterRange::single_value() const {
    return is_interval() ? std::nullopt : first<Expr>(*this);
}
std::optional<Token> Declarator::name() const { return identifier(*this); }
std::optional<Expr> Declarator::initializer() const { return after<Expr>(*this, {"="}); }
std::vector<ArrayRange> Declarator::array_ranges() const { return all<ArrayRange>(*this); }
std::vector<ParameterRange> Declarator::parameter_ranges() const { return all<ParameterRange>(*this); }
std::optional<Token> Port::name() const {
    // ANSI 端口前面可以有 discipline；真正名字是 '=' 前最后一个直接名称。
    std::optional<Token> result;
    for (const auto& token : tokens()) {
        if (token.text() == "=") break;
        if (is_name(token.kind())) result = token;
    }
    return result;
}
std::optional<Token> Port::direction() const { return matching(*this, {"input", "output", "inout"}); }
std::optional<Token> Port::data_type() const {
    const auto port_name = name();
    for (const auto& token : tokens()) {
        if (port_name && token == *port_name) break;
        if (is_type(token.text()) || is_name(token.kind())) return token;
    }
    return std::nullopt;
}
std::optional<Expr> Port::default_value() const { return after<Expr>(*this, {"="}); }
std::vector<ArrayRange> Port::array_ranges() const { return all<ArrayRange>(*this); }
std::vector<Token> BranchDecl::names() const {
    std::vector<Token> result;
    for (const auto& token : tokens()) {
        if (is_name(token.kind())) result.push_back(token);
    }
    return result;
}
std::vector<Expr> BranchDecl::terminals() const {
    const auto list = first<ArgumentList>(*this);
    return list ? list->arguments() : std::vector<Expr>{};
}
std::optional<Token> AnalogFunction::name() const { return identifier(*this); }
std::optional<Token> AnalogFunction::return_type() const {
    return direct_token(*this, [](const Token& token) { return is_type(token.text()); });
}
std::vector<Port> AnalogFunction::ports() const { return ports_of(*this); }
std::vector<Declaration> AnalogFunction::declarations() const { return all<Declaration>(*this); }
std::vector<Statement> AnalogFunction::statements() const { return all<Statement>(*this); }

bool Assignment::can_cast(SyntaxKind kind) {
    return kind == SyntaxKind::AssignmentStmt || kind == SyntaxKind::ContributionStmt;
}
Assignment::Assignment(Node node) : Statement(std::move(node)) {
    if (!can_cast(kind())) throw std::invalid_argument("AST View requires an assignment or contribution");
}
std::optional<Token> Assignment::op() const {
    return matching(*this, {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<+"});
}
std::optional<Expr> Assignment::lhs() const { return operand_before(*this, op()); }
std::optional<Expr> Assignment::rhs() const { return operand_after(*this, op()); }
std::optional<Expr> ExprStmt::expression() const { return first<Expr>(*this); }
std::optional<Token> BlockStmt::label() const {
    const auto colon = position(*this, {":"});
    const auto end = position(*this, {"end"});
    return colon && (!end || *colon < *end) ? identifier(*this, *colon + 1, end.value_or(unlimited)) :
                                            std::nullopt;
}
std::vector<Node> BlockStmt::items() const {
    auto result = children();
    std::erase_if(result, [](const Node& child) { return child.kind() == SyntaxKind::AttributeList; });
    return result;
}
std::vector<Statement> BlockStmt::statements() const { return all<Statement>(*this); }
std::vector<Declaration> BlockStmt::declarations() const { return all<Declaration>(*this); }
std::optional<Expr> IfStmt::condition() const { return between<Expr>(*this, "(", ")"); }
std::optional<Statement> IfStmt::then_branch() const {
    const auto close = position(*this, {")"});
    const auto other = position(*this, {"else"});
    return close ? first<Statement>(*this, *close + 1, other.value_or(unlimited)) : std::nullopt;
}
std::optional<Statement> IfStmt::else_branch() const { return after<Statement>(*this, {"else"}); }
std::optional<Statement> ForStmt::initializer() const { return between<Statement>(*this, "(", ";"); }
std::optional<Expr> ForStmt::condition() const {
    const auto begin = position(*this, {";"});
    const auto end = position(*this, {";"}, 1);
    return begin && end ? first<Expr>(*this, *begin + 1, *end) : std::nullopt;
}
std::optional<Statement> ForStmt::step() const {
    const auto begin = position(*this, {";"}, 1);
    const auto end = position(*this, {")"});
    return begin && end ? first<Statement>(*this, *begin + 1, *end) : std::nullopt;
}
std::optional<Statement> ForStmt::body() const { return after<Statement>(*this, {")"}); }
std::optional<Expr> WhileStmt::condition() const { return between<Expr>(*this, "(", ")"); }
std::optional<Statement> WhileStmt::body() const { return after<Statement>(*this, {")"}); }
std::optional<Expr> RepeatStmt::count() const { return between<Expr>(*this, "(", ")"); }
std::optional<Statement> RepeatStmt::body() const { return after<Statement>(*this, {")"}); }
std::vector<Expr> EventList::events() const { return all<Expr>(*this); }
std::vector<Expr> EventStmt::events() const {
    const auto list = first<EventList>(*this);
    return list ? list->events() : std::vector<Expr>{};
}
std::optional<Statement> EventStmt::body() const {
    bool past_list = false;
    for (const auto& child : children()) {
        if (child.kind() == SyntaxKind::EventList) {
            past_list = true;
        } else if (past_list) {
            if (auto value = cast<Statement>(child)) return value;
        }
    }
    return std::nullopt;
}
bool CaseItem::is_default() const { return position(*this, {"default"}).has_value(); }
std::vector<Expr> CaseItem::labels() const {
    const auto colon = position(*this, {":"});
    return !is_default() && colon ? all<Expr>(*this, 0, *colon) : std::vector<Expr>{};
}
std::optional<Statement> CaseItem::body() const {
    const auto colon = position(*this, {":"});
    if (colon) return first<Statement>(*this, *colon + 1);
    return is_default() ? after<Statement>(*this, {"default"}) : std::nullopt;
}
std::optional<Expr> CaseStmt::selector() const { return between<Expr>(*this, "(", ")"); }
std::vector<CaseItem> CaseStmt::items() const { return all<CaseItem>(*this); }
std::optional<Token> JumpStmt::keyword() const { return first_direct(*this); }
std::optional<Expr> JumpStmt::value() const { return first<Expr>(*this); }
bool AnalogBlock::is_initial() const { return position(*this, {"initial"}).has_value(); }
std::optional<Statement> AnalogBlock::body() const { return first<Statement>(*this); }
std::optional<Token> PropertyDecl::name() const { return identifier(*this); }
std::optional<Expr> PropertyDecl::value() const { return first<Expr>(*this); }
std::optional<Token> NatureDecl::name() const { return identifier(*this); }
std::optional<Token> NatureDecl::parent_name() const {
    const auto colon = position(*this, {":"});
    return colon ? identifier(*this, *colon + 1) : std::nullopt;
}
std::vector<PropertyDecl> NatureDecl::properties() const { return all<PropertyDecl>(*this); }
std::optional<Token> DisciplineDecl::name() const { return identifier(*this); }
std::vector<PropertyDecl> DisciplineDecl::properties() const { return all<PropertyDecl>(*this); }
std::optional<Token> Module::name() const { return identifier(*this); }
std::vector<Port> Module::ports() const { return ports_of(*this); }
std::vector<ParameterDecl> Module::parameters() const {
    std::vector<ParameterDecl> result;
    for (const auto& child : children()) {
        if (child.kind() == SyntaxKind::ModuleParameterList) {
            const auto parameters = all<ParameterDecl>(child);
            result.insert(result.end(), parameters.begin(), parameters.end());
        } else if (const auto parameter = cast<ParameterDecl>(child)) {
            result.push_back(*parameter);
        }
    }
    return result;
}
std::vector<Declaration> Module::declarations() const { return all<Declaration>(*this); }
std::vector<BranchDecl> Module::branches() const { return all<BranchDecl>(*this); }
std::vector<AnalogBlock> Module::analog_blocks() const { return all<AnalogBlock>(*this); }
std::vector<AnalogFunction> Module::functions() const { return all<AnalogFunction>(*this); }
std::vector<Node> Module::items() const {
    auto result = children();
    std::erase_if(result, [](const Node& child) {
        return child.kind() == SyntaxKind::AttributeList || child.kind() == SyntaxKind::ModulePortList ||
               child.kind() == SyntaxKind::ModuleParameterList;
    });
    return result;
}
Root Root::from_tree(const SyntaxTree& tree) { return Root(Node(tree, tree.root())); }
std::vector<Module> Root::modules() const { return all<Module>(*this); }
std::vector<NatureDecl> Root::natures() const { return all<NatureDecl>(*this); }
std::vector<DisciplineDecl> Root::disciplines() const { return all<DisciplineDecl>(*this); }

} // namespace vagrad::ast
