#pragma once

#include "vagrad/common/source_span.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vagrad {

std::string path_utf8(const std::filesystem::path& path);

// 文件在加载时建立不可变快照。保留 shared_ptr 即可延长文字和行索引的寿命。
class SourceFile {
public:
    SourceFile(FileId id, std::filesystem::path path, std::string text);

    FileId id() const { return id_; }
    const std::filesystem::path& path() const { return path_; }
    const std::string& text() const { return text_; }
    std::size_t line_count() const { return line_starts_.size(); }

    SourceLocation locate(std::size_t byte_offset) const;
    std::string_view slice(std::size_t begin, std::size_t end) const;
    std::string_view line_text(std::size_t one_based_line) const;

private:
    FileId id_;
    std::filesystem::path path_;
    std::string text_;
    std::vector<std::size_t> line_starts_;
};

class SourceManager {
public:
    // 规范化路径用于文件去重。同一管理器中重复加载会返回已保存的快照。
    FileId load(const std::filesystem::path& path);
    // 命令行宏也保存为只读源码，使其 token 具有可回查的原始范围。
    FileId add_virtual(std::string name, std::string text);
    std::shared_ptr<const SourceFile> get(FileId id) const;
    std::string_view text(SourceSpan span) const;
    std::size_t file_count() const { return files_.size(); }

private:
    FileId append_file(std::filesystem::path path, std::string text);
    std::vector<std::shared_ptr<const SourceFile>> files_;
    std::map<std::filesystem::path, FileId> by_path_;
};

} // namespace vagrad

