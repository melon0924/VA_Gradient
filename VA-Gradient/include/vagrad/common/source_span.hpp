#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace vagrad {

// 强类型 ID 避免将文件编号和表达式等其他编号混用。
struct FileId {
    std::uint32_t value = std::numeric_limits<std::uint32_t>::max();
    friend bool operator==(FileId, FileId) = default;
};

// 原始文件内的字节范围 [begin, end)，允许文件末尾的空范围。
struct SourceSpan {
    FileId file;
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct SourceLocation {
    std::size_t line = 1;
    std::size_t column = 1;
};

} // namespace vagrad

