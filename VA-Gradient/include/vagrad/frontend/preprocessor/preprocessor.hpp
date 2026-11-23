#pragma once

#include "vagrad/common/diagnostic.hpp"
#include "vagrad/common/source_manager.hpp"
#include "vagrad/frontend/lexer/token.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vagrad {

struct PreprocessOptions {
    std::vector<std::filesystem::path> include_dirs;
    std::map<std::string, std::string> defines;
};

enum class ExpansionKind { Root, Include, Macro, Argument, CommandLine, Generated };

// 同一种上下文结构记录 include、宏调用、实参替换和它们之间的来源关系。
struct ExpansionContext {
    FileId file;
    std::optional<std::size_t> parent;
    std::optional<SourceSpan> call_site;
    ExpansionKind kind = ExpansionKind::Root;
    std::string macro_name = {};
    std::optional<SourceSpan> definition = std::nullopt;
    std::optional<std::size_t> argument_origin = std::nullopt;
    std::optional<std::size_t> definition_origin = std::nullopt;
};

struct ExpandedToken {
    Token original;
    std::size_t context;
    // 展开流中的位置与原始文件位置分别保存。
    std::size_t begin;
    std::size_t end;
};

struct PreprocessResult {
    std::map<std::uint32_t, std::shared_ptr<const SourceFile>> files;
    std::vector<ExpansionContext> contexts;
    std::vector<ExpandedToken> tokens;
    std::vector<Diagnostic> diagnostics;

    bool has_errors() const;
};

// include、宏和条件编译统一在这里按源码顺序处理。
PreprocessResult preprocess(SourceManager& sources, FileId entry,
                            const PreprocessOptions& options = {});
void print_preprocessed_source(std::ostream& out, const PreprocessResult& result);
void print_source_map(std::ostream& out, const PreprocessResult& result);
void print_preprocessed_tokens(std::ostream& out, const PreprocessResult& result);

} // namespace vagrad

