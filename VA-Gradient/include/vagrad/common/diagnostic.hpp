#pragma once

#include "vagrad/common/source_span.hpp"

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace vagrad {

class SourceManager;

enum class Severity { Note, Warning, Error };

struct RelatedLocation {
    SourceSpan span;
    std::string message;
};

struct Diagnostic {
    Severity severity = Severity::Error;
    std::string code;
    std::string message;
    std::optional<SourceSpan> primary;
    std::vector<RelatedLocation> related;
};

void print_diagnostic(std::ostream& out, const SourceManager& sources,
                      const Diagnostic& diagnostic);

} // namespace vagrad

