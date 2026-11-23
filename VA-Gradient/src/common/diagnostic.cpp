#include "vagrad/common/diagnostic.hpp"
#include "vagrad/common/source_manager.hpp"

#include <ostream>
#include <stdexcept>

namespace vagrad {
namespace {

const char* severity_name(Severity severity) {
    switch (severity) {
    case Severity::Note: return "note";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
    }
    throw std::logic_error("invalid diagnostic severity");
}

void print_location(std::ostream& out, const SourceManager& sources, SourceSpan span) {
    const auto file = sources.get(span.file);
    file->slice(span.begin, span.end);
    const auto location = file->locate(span.begin);
    out << path_utf8(file->path()) << ':' << location.line << ':' << location.column << ": ";
}

} // namespace

void print_diagnostic(std::ostream& out, const SourceManager& sources,
                      const Diagnostic& diagnostic) {
    if (diagnostic.primary) {
        print_location(out, sources, *diagnostic.primary);
    }
    out << severity_name(diagnostic.severity) << '[' << diagnostic.code << "]: "
        << diagnostic.message << '\n';
    for (const auto& related : diagnostic.related) {
        print_location(out, sources, related.span);
        out << "note: " << related.message << '\n';
    }
}

} // namespace vagrad

