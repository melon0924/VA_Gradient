#pragma once

#include "vagrad/frontend/lexer/token.hpp"

#include <map>
#include <string>
#include <vector>

namespace vagrad {

struct MacroToken {
    Token original;
    std::size_t context;
};

struct Macro {
    std::string name;
    bool function_like = false;
    std::vector<std::string> parameters;
    std::vector<MacroToken> body;
    SourceSpan definition;
    std::size_t definition_context = 0;
};

using MacroTable = std::map<std::string, Macro>;

} // namespace vagrad

