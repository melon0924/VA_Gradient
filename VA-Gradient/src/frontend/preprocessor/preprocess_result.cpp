#include "vagrad/frontend/preprocessor/preprocessor.hpp"
#include "vagrad/frontend/lexer/lexer.hpp"

#include <algorithm>
#include <iomanip>
#include <ostream>

namespace vagrad {

bool PreprocessResult::has_errors() const {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

void print_preprocessed_source(std::ostream& out, const PreprocessResult& result) {
    for (const auto& token : result.tokens) {
        const auto span = token.original.span;
        out << result.files.at(span.file.value)->slice(span.begin, span.end);
    }
}

void print_source_map(std::ostream& out, const PreprocessResult& result) {
    for (const auto& [id, file] : result.files) {
        out << "File[" << id << "] " << std::quoted(path_utf8(file->path())) << '\n';
    }
    for (std::size_t i = 0; i < result.contexts.size(); ++i) {
        const auto& context = result.contexts[i];
        out << "Context[" << i << "] File[" << context.file.value << ']';
        if (context.kind == ExpansionKind::CommandLine) {
            out << " CommandLine";
        } else if (context.kind == ExpansionKind::Generated) {
            out << " GeneratedSeparator";
        }
        if (!context.macro_name.empty()) {
            out << (context.kind == ExpansionKind::Argument ? " Argument " : " Macro ")
                << std::quoted(context.macro_name);
        }
        if (context.parent) {
            const auto& site = *context.call_site;
            const auto location = result.files.at(site.file.value)->locate(site.begin);
            out << " Parent[" << *context.parent << "] at File[" << site.file.value << "] "
                << location.line << ':' << location.column;
        } else {
            out << " root";
        }
        if (context.definition) {
            const auto& span = *context.definition;
            const auto location = result.files.at(span.file.value)->locate(span.begin);
            out << (context.kind == ExpansionKind::Argument ? " Formal at File[" :
                                                             " Defined at File[")
                << span.file.value << "] " << location.line << ':' << location.column;
        }
        if (context.argument_origin) {
            out << " OriginContext[" << *context.argument_origin << ']';
        }
        if (context.definition_origin) {
            out << " DefinitionContext[" << *context.definition_origin << ']';
        }
        out << '\n';
    }
}

void print_preprocessed_tokens(std::ostream& out, const PreprocessResult& result) {
    print_source_map(out, result);
    for (const auto& token : result.tokens) {
        const auto& original = token.original;
        out << "Expanded[" << token.begin << ',' << token.end << ") File["
            << original.span.file.value << "] Context[" << token.context << "] ";
        print_token(out, *result.files.at(original.span.file.value), original);
        out << '\n';
    }
}

} // namespace vagrad
