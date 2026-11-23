#include "vagrad/frontend/parser/parser.hpp"
#include "vagrad/frontend/lexer/lexer.hpp"

#include <algorithm>
#include <iomanip>
#include <ostream>

namespace vagrad {

bool ParseResult::has_errors() const {
    return input->has_errors() ||
        std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.severity == Severity::Error;
        });
}

void print_parse_events(std::ostream& out, const ParseResult& result) {
    print_source_map(out, *result.input);
    for (std::size_t i = 0; i < result.events.size(); ++i) {
        const auto& event = result.events[i];
        out << "Event[" << i << "] ";
        switch (event.kind) {
        case ParseEventKind::StartNode:
            out << "StartNode(" << syntax_kind_name(event.node_kind) << ')';
            if (event.forward_parent) {
                out << " ForwardParent[" << i + event.forward_parent << ']';
            }
            break;
        case ParseEventKind::FinishNode:
            out << "FinishNode";
            break;
        case ParseEventKind::Token: {
            const auto& token = result.input->tokens.at(event.token_index);
            out << "Token[" << event.token_index << "] Expanded[" << token.begin << ',' << token.end
                << ") File[" << token.original.span.file.value << "] Context[" << token.context << "] ";
            print_token(out, *result.input->files.at(token.original.span.file.value), token.original);
            break;
        }
        case ParseEventKind::Error:
            out << "Error[" << event.diagnostic_index << "] "
                << result.diagnostics.at(event.diagnostic_index).code << ' '
                << std::quoted(result.diagnostics.at(event.diagnostic_index).message);
            break;
        }
        out << '\n';
    }
}

} // namespace vagrad
