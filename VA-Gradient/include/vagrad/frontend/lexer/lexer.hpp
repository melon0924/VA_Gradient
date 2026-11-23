#pragma once

#include "vagrad/common/diagnostic.hpp"
#include "vagrad/common/source_manager.hpp"
#include "vagrad/frontend/lexer/token.hpp"

#include <iosfwd>
#include <memory>
#include <string_view>
#include <vector>

namespace vagrad {

struct LexResult {
    std::shared_ptr<const SourceFile> source;
    std::vector<Token> tokens;
    std::vector<Diagnostic> diagnostics;

    std::string_view text(const Token& token) const;
    bool has_errors() const;
};

// 即使发生词法错误，也保留相应 Error token 并继续扫描。
// tokens 连续覆盖整个输入，最后一个 EOF token 位于文件末尾。
LexResult lex(std::shared_ptr<const SourceFile> source);
// 打印一个 token，供单文件和多文件输出共用，末尾换行由调用方添加。
void print_token(std::ostream& out, const SourceFile& source, const Token& token);
void print_tokens(std::ostream& out, const LexResult& result);

} // namespace vagrad

