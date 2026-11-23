#pragma once

#include "vagrad/common/source_manager.hpp"

#include <filesystem>
#include <string_view>
#include <vector>

namespace vagrad {

FileId resolve_include(SourceManager& sources, const SourceFile& including,
                       std::string_view quoted_name,
                       const std::vector<std::filesystem::path>& search_dirs = {});

} // namespace vagrad

