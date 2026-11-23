#include "vagrad/frontend/ast/ast.hpp"

#include <ostream>
#include <string>

namespace vagrad::ast {
namespace {

void quote(std::ostream& out, std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char ch : text) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20 || ch == 0x7f) {
                out << "\\x" << hex[ch >> 4] << hex[ch & 0x0f];
            } else {
                out.put(static_cast<char>(ch));
            }
        }
    }
    out << '"';
}
void field(std::ostream& out, std::string_view key, const std::optional<Token>& token) {
    if (token) {
        out << ' ' << key << '=';
        quote(out, token->text());
    }
}

// 标签由类型化访问器取得，导出器无需猜测原始 children[0]/children[1]。
template<class T>
bool refers(const Node& child, const std::optional<T>& value) {
    return value && child == *value;
}
std::string_view role(const Node& parent, const Node& child) {
    if (const auto assignment = cast<Assignment>(parent)) {
        if (refers(child, assignment->lhs())) return "lhs";
        if (refers(child, assignment->rhs())) return "rhs";
    } else if (const auto binaryExpr = cast<BinaryExpr>(parent)) {
        if (refers(child, binaryExpr->lhs())) return "lhs";
        if (refers(child, binaryExpr->rhs())) return "rhs";
    } else if (const auto unaryExpr = cast<UnaryExpr>(parent)) {
        if (refers(child, unaryExpr->operand())) return "operand";
    } else if (const auto postfixExpr = cast<PostfixExpr>(parent)) {
        if (refers(child, postfixExpr->operand())) return "operand";
    } else if (const auto conditionalExpr = cast<ConditionalExpr>(parent)) {
        if (refers(child, conditionalExpr->condition())) return "condition";
        if (refers(child, conditionalExpr->true_value())) return "true_value";
        if (refers(child, conditionalExpr->false_value())) return "false_value";
    } else if (const auto callExpr = cast<CallExpr>(parent)) {
        if (refers(child, callExpr->callee())) return "callee";
        if (child.kind() == SyntaxKind::ArgumentList) return "arguments";
    } else if (const auto parenExpr = cast<ParenExpr>(parent)) {
        if (refers(child, parenExpr->inner())) return "inner";
    } else if (const auto indexExpr = cast<IndexExpr>(parent)) {
        if (refers(child, indexExpr->base())) return "base";
        if (refers(child, indexExpr->index())) return "index";
        if (refers(child, indexExpr->range_end())) return "range_end";
    } else if (const auto memberExpr = cast<MemberExpr>(parent)) {
        if (refers(child, memberExpr->base())) return "base";
    } else if (const auto replicationExpr = cast<ReplicationExpr>(parent)) {
        if (refers(child, replicationExpr->count())) return "count";
        if (refers(child, replicationExpr->value())) return "value";
    } else if (const auto declarator = cast<Declarator>(parent)) {
        if (refers(child, declarator->initializer())) return "initializer";
    } else if (const auto parameterRange = cast<ParameterRange>(parent)) {
        if (refers(child, parameterRange->lower())) return "lower";
        if (refers(child, parameterRange->upper())) return "upper";
        if (refers(child, parameterRange->single_value())) return "single_value";
    } else if (const auto arrayRange = cast<ArrayRange>(parent)) {
        if (refers(child, arrayRange->left())) return "left";
        if (refers(child, arrayRange->right())) return "right";
    } else if (const auto ifStmt = cast<IfStmt>(parent)) {
        if (refers(child, ifStmt->condition())) return "condition";
        if (refers(child, ifStmt->then_branch())) return "then_branch";
        if (refers(child, ifStmt->else_branch())) return "else_branch";
    } else if (const auto forStmt = cast<ForStmt>(parent)) {
        if (refers(child, forStmt->initializer())) return "initializer";
        if (refers(child, forStmt->condition())) return "condition";
        if (refers(child, forStmt->step())) return "step";
        if (refers(child, forStmt->body())) return "body";
    } else if (const auto whileStmt = cast<WhileStmt>(parent)) {
        if (refers(child, whileStmt->condition())) return "condition";
        if (refers(child, whileStmt->body())) return "body";
    } else if (const auto repeatStmt = cast<RepeatStmt>(parent)) {
        if (refers(child, repeatStmt->count())) return "count";
        if (refers(child, repeatStmt->body())) return "body";
    } else if (const auto caseStmt = cast<CaseStmt>(parent)) {
        if (refers(child, caseStmt->selector())) return "selector";
    } else if (const auto caseItem = cast<CaseItem>(parent)) {
        if (refers(child, caseItem->body())) return "body";
    } else if (const auto eventStmt = cast<EventStmt>(parent)) {
        if (refers(child, eventStmt->body())) return "body";
    } else if (const auto analogBlock = cast<AnalogBlock>(parent)) {
        if (refers(child, analogBlock->body())) return "body";
    } else if (const auto exprStmt = cast<ExprStmt>(parent)) {
        if (refers(child, exprStmt->expression())) return "expression";
    } else if (const auto attribute = cast<Attribute>(parent)) {
        if (refers(child, attribute->value())) return "value";
    } else if (const auto propertyDecl = cast<PropertyDecl>(parent)) {
        if (refers(child, propertyDecl->value())) return "value";
    } else if (const auto port = cast<Port>(parent)) {
        if (refers(child, port->default_value())) return "default_value";
    }
    return {};
}

void fields(std::ostream& out, const Node& node) {
    if (const auto module = cast<Module>(node)) {
        field(out, "name", module->name());
    } else if (const auto declaration = cast<Declaration>(node)) {
        field(out, "keyword", declaration->keyword());
        field(out, "type", declaration->data_type());
    } else if (const auto declarator = cast<Declarator>(node)) {
        field(out, "name", declarator->name());
    } else if (const auto port = cast<Port>(node)) {
        field(out, "name", port->name());
        field(out, "direction", port->direction());
        field(out, "type", port->data_type());
    } else if (const auto branchDecl = cast<BranchDecl>(node)) {
        out << " names=[";
        bool first = true;
        for (const auto& name : branchDecl->names()) {
            if (!first) out << ',';
            quote(out, name.text());
            first = false;
        }
        out << "] terminals=" << branchDecl->terminals().size();
    } else if (const auto analogFunction = cast<AnalogFunction>(node)) {
        field(out, "name", analogFunction->name());
        field(out, "return_type", analogFunction->return_type());
    } else if (const auto natureDecl = cast<NatureDecl>(node)) {
        field(out, "name", natureDecl->name());
        field(out, "parent", natureDecl->parent_name());
    } else if (const auto disciplineDecl = cast<DisciplineDecl>(node)) {
        field(out, "name", disciplineDecl->name());
    } else if (const auto propertyDecl = cast<PropertyDecl>(node)) {
        field(out, "name", propertyDecl->name());
    } else if (const auto attribute = cast<Attribute>(node)) {
        field(out, "name", attribute->name());
    } else if (const auto parameterRange = cast<ParameterRange>(node)) {
        field(out, "keyword", parameterRange->keyword());
        if (parameterRange->is_interval()) {
            out << " lower_inclusive=" << (parameterRange->lower_inclusive().value_or(false) ? "true" : "false");
            if (const auto upper = parameterRange->upper_inclusive()) {
                out << " upper_inclusive=" << (*upper ? "true" : "false");
            }
        }
    } else if (const auto nameExpr = cast<NameExpr>(node)) {
        field(out, "name", nameExpr->name());
    } else if (const auto literalExpr = cast<LiteralExpr>(node)) {
        field(out, "value", literalExpr->literal());
    } else if (const auto assignment = cast<Assignment>(node)) {
        field(out, "op", assignment->op());
    } else if (const auto binaryExpr = cast<BinaryExpr>(node)) {
        field(out, "op", binaryExpr->op());
    } else if (const auto unaryExpr = cast<UnaryExpr>(node)) {
        field(out, "op", unaryExpr->op());
    } else if (const auto postfixExpr = cast<PostfixExpr>(node)) {
        field(out, "op", postfixExpr->op());
    } else if (const auto memberExpr = cast<MemberExpr>(node)) {
        field(out, "member", memberExpr->member());
    } else if (const auto indexExpr = cast<IndexExpr>(node)) {
        field(out, "separator", indexExpr->separator());
    } else if (const auto blockStmt = cast<BlockStmt>(node)) {
        field(out, "label", blockStmt->label());
    } else if (const auto caseItem = cast<CaseItem>(node)) {
        out << " default=" << (caseItem->is_default() ? "true" : "false");
    } else if (const auto analogBlock = cast<AnalogBlock>(node)) {
        out << " initial=" << (analogBlock->is_initial() ? "true" : "false");
    } else if (const auto jumpStmt = cast<JumpStmt>(node)) {
        field(out, "keyword", jumpStmt->keyword());
    } else if (node.kind() == SyntaxKind::ErrorNode) {
        out << " missing=" << (node.range().begin == node.range().end ? "true" : "false");
    }
}

} // namespace

void print_ast_view(std::ostream& out, const SyntaxTree& tree) {
    print_source_map(out, tree.input());
    struct Work { Node node; std::size_t depth; std::string label; };
    std::vector<Work> pending{{Root::from_tree(tree), 0, {}}};
    while (!pending.empty()) {
        auto work = std::move(pending.back());
        pending.pop_back();
        const auto& node = work.node;
        out << std::string(work.depth * 2, ' ');
        if (!work.label.empty()) out << work.label << ": ";
        out << syntax_kind_name(node.kind());
        fields(out, node);
        const auto range = node.range();
        out << " Node[" << node.id().value << "] Expanded[" << range.begin << ',' << range.end << ')';
        if (const auto origin = node.first_token()) {
            const auto span = origin->source_span();
            const auto location = tree.input().files.at(span.file.value)->locate(span.begin);
            out << " Origin(File[" << span.file.value << "] " << location.line << ':' << location.column
                << " Context[" << origin->context_index() << "])";
        }
        out << '\n';
        const auto children = node.children();
        for (auto i = children.rbegin(); i != children.rend(); ++i) {
            pending.push_back({*i, work.depth + 1, std::string(role(node, *i))});
        }
    }
}

} // namespace vagrad::ast
