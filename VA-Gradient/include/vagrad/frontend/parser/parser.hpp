#pragma once

#include "vagrad/frontend/parser/parse_event.hpp"
#include "vagrad/frontend/preprocessor/preprocessor.hpp"

#include <iosfwd>
#include <memory>
#include <vector>

namespace vagrad {

struct ParseResult {
    // 与预处理器共享只读结果，Token 事件继续引用原来的 token 和来源图。
    std::shared_ptr<const PreprocessResult> input;
    std::vector<ParseEvent> events;
    std::vector<Diagnostic> diagnostics; // 本阶段的语法错误

    bool has_errors() const;
};

ParseResult parse(std::shared_ptr<const PreprocessResult> input);
void print_parse_events(std::ostream& out, const ParseResult& result);

} // namespace vagrad
