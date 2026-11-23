#pragma once

#include <string_view>

namespace vagrad {

// 原始 token 保留空白和注释，供后续无损 CST 使用。
enum class TokenKind {
    EndOfFile,
    ByteOrderMark,
    Whitespace,
    Newline,
    LineComment,
    BlockComment,
    LineContinuation,
    Identifier,
    EscapedIdentifier,
    SystemIdentifier,
    Keyword,
    Directive,
    NumberLiteral,
    StringLiteral,
    Symbol,
    Error
};

std::string_view token_kind_name(TokenKind kind);
bool is_trivia(TokenKind kind);

} // namespace vagrad

