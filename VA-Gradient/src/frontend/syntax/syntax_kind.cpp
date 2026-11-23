#include "vagrad/frontend/syntax/syntax_kind.hpp"

namespace vagrad {

std::string_view syntax_kind_name(SyntaxKind kind) {
    switch (kind) {
#define VAGRAD_SYNTAX_NAME(name) case SyntaxKind::name: return #name;
    VAGRAD_SYNTAX_NAME(Tombstone)
    VAGRAD_SYNTAX_NAME(Root)
    VAGRAD_SYNTAX_NAME(ModuleDecl)
    VAGRAD_SYNTAX_NAME(ModulePortList)
    VAGRAD_SYNTAX_NAME(Port)
    VAGRAD_SYNTAX_NAME(ModuleParameterList)
    VAGRAD_SYNTAX_NAME(PortDecl)
    VAGRAD_SYNTAX_NAME(NetDecl)
    VAGRAD_SYNTAX_NAME(BranchDecl)
    VAGRAD_SYNTAX_NAME(ParameterDecl)
    VAGRAD_SYNTAX_NAME(VariableDecl)
    VAGRAD_SYNTAX_NAME(Declarator)
    VAGRAD_SYNTAX_NAME(ParameterRange)
    VAGRAD_SYNTAX_NAME(ArrayRange)
    VAGRAD_SYNTAX_NAME(AttributeList)
    VAGRAD_SYNTAX_NAME(Attribute)
    VAGRAD_SYNTAX_NAME(NatureDecl)
    VAGRAD_SYNTAX_NAME(DisciplineDecl)
    VAGRAD_SYNTAX_NAME(PropertyDecl)
    VAGRAD_SYNTAX_NAME(AnalogFunctionDecl)
    VAGRAD_SYNTAX_NAME(AnalogBlock)
    VAGRAD_SYNTAX_NAME(BlockStmt)
    VAGRAD_SYNTAX_NAME(IfStmt)
    VAGRAD_SYNTAX_NAME(CaseStmt)
    VAGRAD_SYNTAX_NAME(CaseItem)
    VAGRAD_SYNTAX_NAME(ForStmt)
    VAGRAD_SYNTAX_NAME(WhileStmt)
    VAGRAD_SYNTAX_NAME(RepeatStmt)
    VAGRAD_SYNTAX_NAME(EventStmt)
    VAGRAD_SYNTAX_NAME(EventList)
    VAGRAD_SYNTAX_NAME(AssignmentStmt)
    VAGRAD_SYNTAX_NAME(ContributionStmt)
    VAGRAD_SYNTAX_NAME(ExprStmt)
    VAGRAD_SYNTAX_NAME(EmptyStmt)
    VAGRAD_SYNTAX_NAME(JumpStmt)
    VAGRAD_SYNTAX_NAME(NameExpr)
    VAGRAD_SYNTAX_NAME(LiteralExpr)
    VAGRAD_SYNTAX_NAME(UnaryExpr)
    VAGRAD_SYNTAX_NAME(BinaryExpr)
    VAGRAD_SYNTAX_NAME(ConditionalExpr)
    VAGRAD_SYNTAX_NAME(ParenExpr)
    VAGRAD_SYNTAX_NAME(CallExpr)
    VAGRAD_SYNTAX_NAME(ArgumentList)
    VAGRAD_SYNTAX_NAME(IndexExpr)
    VAGRAD_SYNTAX_NAME(MemberExpr)
    VAGRAD_SYNTAX_NAME(PostfixExpr)
    VAGRAD_SYNTAX_NAME(ConcatExpr)
    VAGRAD_SYNTAX_NAME(ReplicationExpr)
    VAGRAD_SYNTAX_NAME(ErrorNode)
#undef VAGRAD_SYNTAX_NAME
    }
    return "Unknown";
}

} // namespace vagrad
