#include "vagrad/common/source_manager.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vagrad {

std::string path_utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}

SourceFile::SourceFile(FileId id, std::filesystem::path path, std::string text)
    : id_(id), path_(std::move(path)), text_(std::move(text)), line_starts_{0} {
    for (std::size_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\r') {
            if (i + 1 < text_.size() && text_[i + 1] == '\n') {
                ++i;
            }
            line_starts_.push_back(i + 1);
        } else if (text_[i] == '\n') {
            line_starts_.push_back(i + 1);
        }
    }
}

SourceLocation SourceFile::locate(std::size_t byte_offset) const {
    if (byte_offset > text_.size()) {
        throw std::out_of_range("source offset exceeds file size");
    }
    const auto next = std::upper_bound(line_starts_.begin(), line_starts_.end(), byte_offset);
    const auto line_index = static_cast<std::size_t>(next - line_starts_.begin() - 1);
    return {line_index + 1, byte_offset - line_starts_[line_index] + 1};
}

std::string_view SourceFile::slice(std::size_t begin, std::size_t end) const {
    if (begin > end || end > text_.size()) {
        throw std::out_of_range("invalid source span");
    }
    return std::string_view(text_).substr(begin, end - begin);
}

std::string_view SourceFile::line_text(std::size_t one_based_line) const {
    if (one_based_line == 0 || one_based_line > line_starts_.size()) {
        throw std::out_of_range("source line does not exist");
    }
    const auto index = one_based_line - 1;
    const auto begin = line_starts_[index];
    auto end = index + 1 < line_starts_.size() ? line_starts_[index + 1] : text_.size();
    while (end > begin && (text_[end - 1] == '\r' || text_[end - 1] == '\n')) {
        --end;
    }
    return slice(begin, end);
}

FileId SourceManager::load(const std::filesystem::path& path) {
    const auto normalized = std::filesystem::canonical(path);
    const auto known = by_path_.find(normalized);
    if (known != by_path_.end()) {
        return known->second;
    }
    if (!std::filesystem::is_regular_file(normalized)) {
        throw std::runtime_error("input path is not a regular file");
    }
    std::ifstream input(normalized, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input file");
    }
    std::string contents(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    if (input.bad()) {
        throw std::runtime_error("failed while reading input file");
    }
    const auto id = append_file(normalized, std::move(contents));
    try {
        by_path_.emplace(normalized, id);
    } catch (...) {
        // 登记失败时撤销刚追加的快照，保证加载可安全重试。
        files_.pop_back();
        throw;
    }
    return id;
}

FileId SourceManager::append_file(std::filesystem::path path, std::string text) {
    if (files_.size() >= std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("too many source files");
    }
    const FileId id{static_cast<std::uint32_t>(files_.size())};
    files_.push_back(std::make_shared<const SourceFile>(id, std::move(path), std::move(text)));
    return id;
}

FileId SourceManager::add_virtual(std::string name, std::string text) {
    return append_file(std::filesystem::path(std::u8string(name.begin(), name.end())),
                       std::move(text));
}

std::shared_ptr<const SourceFile> SourceManager::get(FileId id) const {
    return files_.at(id.value);
}

std::string_view SourceManager::text(SourceSpan span) const {
    return get(span.file)->slice(span.begin, span.end);
}

} // namespace vagrad

