#pragma once

#include "vagrad/common/source_span.hpp"
#include "vagrad/frontend/lexer/token_kind.hpp"

namespace vagrad {

// token 引用文件快照内的字节范围，文字由 LexResult 的 source 持有。
struct Token {
    TokenKind kind;
    SourceSpan span;
};

} // namespace vagrad

