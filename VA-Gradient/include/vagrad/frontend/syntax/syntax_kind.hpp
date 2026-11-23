#pragma once

#include <string_view>

namespace vagrad {

// 这是语法节点的种类；叶子仍使用 Lexer 的 TokenKind。
enum class SyntaxKind {
    Tombstone, Root, ModuleDecl, ModulePortList, Port, ModuleParameterList,
    PortDecl, NetDecl, BranchDecl, ParameterDecl, VariableDecl, Declarator,
    ParameterRange, ArrayRange, AttributeList, Attribute,
    NatureDecl, DisciplineDecl, PropertyDecl, AnalogFunctionDecl, AnalogBlock,
    BlockStmt, IfStmt, CaseStmt, CaseItem, ForStmt, WhileStmt, RepeatStmt,
    EventStmt, EventList, AssignmentStmt, ContributionStmt, ExprStmt, EmptyStmt,
    JumpStmt, NameExpr, LiteralExpr, UnaryExpr, BinaryExpr, ConditionalExpr,
    ParenExpr, CallExpr, ArgumentList, IndexExpr, MemberExpr, PostfixExpr,
    ConcatExpr, ReplicationExpr, ErrorNode
};

std::string_view syntax_kind_name(SyntaxKind kind);

} // namespace vagrad
