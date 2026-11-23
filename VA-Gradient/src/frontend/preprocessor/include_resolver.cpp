#include "vagrad/frontend/preprocessor/include_resolver.hpp"

#include <stdexcept>

namespace vagrad {

FileId resolve_include(SourceManager& sources, const SourceFile& including,
                       std::string_view quoted_name,
                       const std::vector<std::filesystem::path>& search_dirs) {
    if (quoted_name.size() < 2 || quoted_name.front() != '"' || quoted_name.back() != '"') {
        throw std::invalid_argument("include requires a quoted file name");
    }
    std::string name;
    for (std::size_t i = 1; i + 1 < quoted_name.size(); ++i) {
        if (quoted_name[i] == '\\' && i + 2 < quoted_name.size() &&
            (quoted_name[i + 1] == '\\' || quoted_name[i + 1] == '"')) {
            ++i;
        }
        name += quoted_name[i];
    }
    if (name.empty()) {
        throw std::invalid_argument("include file name must not be empty");
    }
    const auto relative = std::filesystem::path(std::u8string(name.begin(), name.end()));
    std::vector<std::filesystem::path> dirs{including.path().parent_path()};
    dirs.insert(dirs.end(), search_dirs.begin(), search_dirs.end());
    for (const auto& dir : dirs) {
        const auto candidate = dir / relative;
        if (std::filesystem::exists(candidate)) {
            return sources.load(candidate);
        }
    }
    throw std::runtime_error("include file not found: " + name);
}

} // namespace vagrad

