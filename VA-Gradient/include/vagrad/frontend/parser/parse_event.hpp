#pragma once

#include "vagrad/frontend/syntax/syntax_kind.hpp"

#include <cstddef>

namespace vagrad {

enum class ParseEventKind { StartNode, Token, FinishNode, Error };

// Parser 只记录操作，不在识别语法时分配树节点。
// 每类事件只使用自己的字段，其余字段保持默认值。
struct ParseEvent {
    ParseEventKind kind;
    SyntaxKind node_kind = SyntaxKind::Tombstone; // StartNode 使用
    std::size_t token_index = 0;                 // Token 使用：展开流索引
    std::size_t diagnostic_index = 0;            // Error 使用：诊断索引
    // StartNode 使用：指向稍后创建的父节点事件，0 表示没有。
    // 使 Pratt 能在读到运算符后，将已经识别的左表达式包进父节点。
    std::size_t forward_parent = 0;
};

} // namespace vagrad
