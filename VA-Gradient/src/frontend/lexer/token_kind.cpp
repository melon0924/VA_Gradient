#include "vagrad/frontend/lexer/token_kind.hpp"

#include <stdexcept>

namespace vagrad {

std::string_view token_kind_name(TokenKind kind) {
    switch (kind) {
    case TokenKind::EndOfFile: return "EndOfFile";
    case TokenKind::ByteOrderMark: return "ByteOrderMark";
    case TokenKind::Whitespace: return "Whitespace";
    case TokenKind::Newline: return "Newline";
    case TokenKind::LineComment: return "LineComment";
    case TokenKind::BlockComment: return "BlockComment";
    case TokenKind::LineContinuation: return "LineContinuation";
    case TokenKind::Identifier: return "Identifier";
    case TokenKind::EscapedIdentifier: return "EscapedIdentifier";
    case TokenKind::SystemIdentifier: return "SystemIdentifier";
    case TokenKind::Keyword: return "Keyword";
    case TokenKind::Directive: return "Directive";
    case TokenKind::NumberLiteral: return "NumberLiteral";
    case TokenKind::StringLiteral: return "StringLiteral";
    case TokenKind::Symbol: return "Symbol";
    case TokenKind::Error: return "Error";
    }
    throw std::logic_error("invalid token kind");
}

bool is_trivia(TokenKind kind) {
    switch (kind) {
    case TokenKind::ByteOrderMark:
    case TokenKind::Whitespace:
    case TokenKind::Newline:
    case TokenKind::LineComment:
    case TokenKind::BlockComment:
    case TokenKind::LineContinuation:
        return true;
    default:
        return false;
    }
}

} // namespace vagrad

