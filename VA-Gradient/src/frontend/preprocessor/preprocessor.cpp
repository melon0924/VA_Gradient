#include "vagrad/frontend/preprocessor/preprocessor.hpp"
#include "vagrad/frontend/preprocessor/include_resolver.hpp"
#include "vagrad/frontend/preprocessor/macro_table.hpp"
#include "vagrad/frontend/lexer/lexer.hpp"

#include <algorithm>
#include <deque>
#include <iterator>
#include <utility>

namespace vagrad {
namespace {

bool word(TokenKind kind) {
    return kind == TokenKind::Identifier || kind == TokenKind::Keyword;
}

bool source_directive(std::string_view name) {
    return name == "`define" || name == "`undef" || name == "`undefineall" ||
           name == "`include" || name == "`ifdef" || name == "`ifndef" ||
           name == "`elsif" || name == "`else" || name == "`endif";
}

bool conditional(std::string_view name) {
    return name == "`ifdef" || name == "`ifndef" || name == "`elsif" ||
           name == "`else" || name == "`endif";
}

// 不把源码串修改成新字符串：每一枚 token 始终引用原始快照中的范围。
class Preprocessor {
public:
    Preprocessor(SourceManager& sources, const PreprocessOptions& options)
        : sources_(sources), options_(options) {}

    PreprocessResult run(FileId entry) {
        const auto root = sources_.get(entry);
        result_.files.emplace(entry.value, root);
        result_.contexts.push_back({entry, std::nullopt, std::nullopt, ExpansionKind::Root});
        command_line_macros(entry);
        process_file(entry, 0);
        const auto end = root->text().size();
        append({{TokenKind::EndOfFile, {entry, end, end}}, 0});
        return std::move(result_);
    }

private:
    struct Condition {
        bool parent_active;
        bool active;
        bool taken;
        bool seen_else;
        SourceSpan opening;
    };

    struct Work {
        MacroToken token;
        // 替换结果重新扫描时，禁止再次进入当前展开链上的宏。
        std::vector<std::string> disabled;
    };

    std::string_view text(const Token& token) const {
        return result_.files.at(token.span.file.value)->slice(token.span.begin, token.span.end);
    }
    std::string_view text(const MacroToken& token) const { return text(token.original); }

    void diagnose(std::size_t context, std::string code, std::string message,
                  SourceSpan span) {
        Diagnostic diagnostic{Severity::Error, std::move(code), std::move(message), span, {}};
        auto current = std::optional<std::size_t>{context};
        while (current) {
            const auto& origin = result_.contexts[*current];
            if (origin.call_site) {
                diagnostic.related.push_back({*origin.call_site,
                    origin.kind == ExpansionKind::Include ? "included from here" :
                                                            "expanded from here"});
            }
            if (origin.definition) {
                diagnostic.related.push_back({*origin.definition, "macro defined here"});
            }
            current = origin.parent;
        }
        result_.diagnostics.push_back(std::move(diagnostic));
    }

    // 相邻宏替换可能把两个 token 意外粘成一个，比如 + 和 + 变成 ++。
    // 只在新形成的边界试扫；需要时插入有独立来源标记的空白。
    void append(MacroToken token) {
        if (!result_.tokens.empty() && token.original.kind != TokenKind::EndOfFile) {
            const auto& previous = result_.tokens.back();
            const auto left = text(previous.original);
            const auto right = text(token);
            const bool original_boundary = previous.context == token.context &&
                previous.original.span.file == token.original.span.file &&
                previous.original.span.end == token.original.span.begin;
            const bool blank = token.original.kind == TokenKind::Whitespace ||
                token.original.kind == TokenKind::Newline ||
                previous.original.kind == TokenKind::Whitespace ||
                previous.original.kind == TokenKind::Newline ||
                previous.original.kind == TokenKind::ByteOrderMark;
            if (!original_boundary && !blank && !left.empty() && !right.empty()) {
                const auto probe = lex(std::make_shared<SourceFile>(
                    FileId{}, "<boundary>", std::string(left) + std::string(right)));
                if (probe.tokens.size() != 3 || probe.tokens[0].span.end != left.size() ||
                    probe.tokens[0].kind != previous.original.kind ||
                    probe.tokens[1].kind != token.original.kind) {
                    separator(token, previous.original.kind == TokenKind::LineComment);
                }
            }
        }
        append_raw(token);
    }

    void append_raw(MacroToken token) {
        const auto end = expanded_end_ + token.original.span.end - token.original.span.begin;
        result_.tokens.push_back({token.original, token.context, expanded_end_, end});
        expanded_end_ = end;
    }

    void separator(const MacroToken& next, bool newline) {
        if (!spacing_) {
            spacing_ = sources_.add_virtual("<preprocessor-spacing>", " \n");
            result_.files.emplace(spacing_->value, sources_.get(*spacing_));
        }
        const auto context = result_.contexts.size();
        result_.contexts.push_back({*spacing_, next.context, next.original.span,
                                    ExpansionKind::Generated});
        const std::size_t begin = newline ? 1 : 0;
        append_raw({{newline ? TokenKind::Newline : TokenKind::Whitespace,
                     {*spacing_, begin, begin + 1}}, context});
    }

    static void trim(std::vector<MacroToken>& tokens) {
        const auto first = std::find_if_not(tokens.begin(), tokens.end(),
            [](const auto& token) { return is_trivia(token.original.kind); });
        tokens.erase(tokens.begin(), first);
        while (!tokens.empty() && is_trivia(tokens.back().original.kind)) {
            tokens.pop_back();
        }
    }

    std::vector<MacroToken> significant(const std::vector<MacroToken>& tokens) const {
        std::vector<MacroToken> result;
        for (const auto& token : tokens) {
            if (!is_trivia(token.original.kind)) {
                result.push_back(token);
            }
        }
        return result;
    }

    void command_line_macros(FileId entry) {
        for (const auto& [name, value] : options_.defines) {
            const auto id = sources_.add_virtual("<command-line:" + name + ">", name + " " + value);
            const auto file = sources_.get(id);
            result_.files.emplace(id.value, file);
            const auto lexed = lex(file);
            const auto context = result_.contexts.size();
            result_.contexts.push_back({id, 0, SourceSpan{entry, 0, 0},
                                        ExpansionKind::CommandLine});
            if (lexed.tokens.empty() || !word(lexed.tokens[0].kind) ||
                lexed.text(lexed.tokens[0]) != name) {
                diagnose(context, "PP_DEFINE_NAME", "invalid command-line macro name",
                         {id, 0, name.size()});
                continue;
            }
            Macro macro{name, false, {}, {}, lexed.tokens[0].span};
            macro.definition_context = context;
            for (const auto& token : lexed.tokens) {
                if (token.span.begin >= name.size() + 1 && token.kind != TokenKind::EndOfFile) {
                    macro.body.push_back({token, context});
                }
            }
            trim(macro.body);
            for (const auto& diagnostic : lexed.diagnostics) {
                diagnose(context, diagnostic.code, diagnostic.message, *diagnostic.primary);
            }
            macros_[name] = std::move(macro);
        }
    }

    // 指令已经按逻辑行拆开；宏名字后紧挨 '(' 才是带参数的定义。
    void define_macro(const std::vector<MacroToken>& operands, const MacroToken& directive) {
        std::size_t i = 0;
        while (i < operands.size() && is_trivia(operands[i].original.kind)) {
            ++i;
        }
        if (i == operands.size() || !word(operands[i].original.kind)) {
            diagnose(directive.context, "PP_DEFINE_NAME", "define requires a macro name",
                     directive.original.span);
            return;
        }
        const auto name_token = operands[i++];
        Macro macro{std::string(text(name_token)), false, {}, {}, name_token.original.span};
        macro.definition_context = directive.context;
        if (i < operands.size() && text(operands[i]) == "(" &&
            name_token.original.span.end == operands[i].original.span.begin) {
            macro.function_like = true;
            ++i;
            bool need_parameter = true;
            while (i < operands.size()) {
                if (is_trivia(operands[i].original.kind)) {
                    ++i;
                    continue;
                }
                if (text(operands[i]) == ")" && (need_parameter == false || macro.parameters.empty())) {
                    ++i;
                    break;
                }
                if (need_parameter && word(operands[i].original.kind)) {
                    const auto parameter = std::string(text(operands[i]));
                    if (std::find(macro.parameters.begin(), macro.parameters.end(), parameter) !=
                        macro.parameters.end()) {
                        diagnose(directive.context, "PP_DEFINE_PARAMS", "duplicate formal parameter",
                                 operands[i].original.span);
                        return;
                    }
                    macro.parameters.push_back(parameter);
                    need_parameter = false;
                } else if (!need_parameter && text(operands[i]) == ",") {
                    need_parameter = true;
                } else {
                    diagnose(directive.context, "PP_DEFINE_PARAMS", "invalid macro parameter list",
                             operands[i].original.span);
                    return;
                }
                ++i;
            }
            if (i == 0 || text(operands[i - 1]) != ")") {
                diagnose(directive.context, "PP_DEFINE_PARAMS", "unterminated macro parameter list",
                         name_token.original.span);
                return;
            }
        }
        for (; i < operands.size(); ++i) {
            if (operands[i].original.kind != TokenKind::LineContinuation) {
                macro.body.push_back(operands[i]);
            }
        }
        trim(macro.body);
        macros_[macro.name] = std::move(macro);
    }

    // 取出括号配对的实参，只有最外层的逗号才分隔实参。
    bool arguments(std::deque<Work>& queue, const Work& call, const Macro& macro,
                   std::vector<std::vector<Work>>& args) {
        auto first = queue.begin();
        while (first != queue.end() && is_trivia(first->token.original.kind)) {
            ++first;
        }
        if (first == queue.end() || text(first->token) != "(") {
            diagnose(call.token.context, "PP_MACRO_ARGS", "function-like macro requires (...)",
                     call.token.original.span);
            return false;
        }
        queue.erase(queue.begin(), std::next(first));
        args.emplace_back();
        std::vector<std::string_view> closing{")"};
        while (!queue.empty()) {
            auto item = std::move(queue.front());
            queue.pop_front();
            const auto spelling = text(item.token);
            if (spelling == "(" || spelling == "[" || spelling == "{") {
                closing.push_back(spelling == "(" ? ")" : spelling == "[" ? "]" : "}");
            } else if (spelling == ")" || spelling == "]" || spelling == "}") {
                if (spelling != closing.back()) {
                    diagnose(call.token.context, "PP_MACRO_ARGS", "mismatched macro argument delimiter",
                             item.token.original.span);
                    return false;
                }
                closing.pop_back();
                if (closing.empty()) {
                    if (macro.parameters.empty() && significant_work(args[0]).empty()) {
                        args.clear();
                    }
                    if (args.size() != macro.parameters.size()) {
                        diagnose(call.token.context, "PP_MACRO_ARITY",
                                 "macro " + macro.name + " expects " +
                                 std::to_string(macro.parameters.size()) + " arguments, got " +
                                 std::to_string(args.size()), call.token.original.span);
                        return false;
                    }
                    return true;
                }
            } else if (spelling == "," && closing.size() == 1) {
                args.emplace_back();
                continue;
            }
            args.back().push_back(std::move(item));
        }
        diagnose(call.token.context, "PP_MACRO_ARGS", "unterminated macro argument list",
                 call.token.original.span);
        return false;
    }

    static std::vector<Work> significant_work(const std::vector<Work>& tokens) {
        std::vector<Work> result;
        for (const auto& item : tokens) {
            if (!is_trivia(item.token.original.kind)) {
                result.push_back(item);
            }
        }
        return result;
    }

    // 替换 token 放回队首继续扫描，而非仅对一个字符串 replace。
    // 因此对象宏展开成函数宏名字后，仍能读到后面的实参括号。
    std::vector<MacroToken> expand(std::deque<Work> queue, std::size_t depth = 0) {
        std::vector<MacroToken> output;
        if (depth > 128) {
            if (!queue.empty()) {
                diagnose(queue.front().token.context, "PP_MACRO_DEPTH",
                         "macro argument nesting exceeds 128", queue.front().token.original.span);
            }
            return output;
        }
        while (!queue.empty()) {
            auto call = std::move(queue.front());
            queue.pop_front();
            if (++work_count_ > 1000000) {
                if (!limit_reported_) {
                    diagnose(call.token.context, "PP_TOKEN_LIMIT", "preprocessing work limit exceeded",
                             call.token.original.span);
                    limit_reported_ = true;
                }
                break;
            }
            if (call.token.original.kind != TokenKind::Directive) {
                output.push_back(call.token);
                continue;
            }
            const auto name = std::string(text(call.token).substr(1));
            const auto found = macros_.find(name);
            if (found == macros_.end()) {
                diagnose(call.token.context, "PP_UNDEFINED_MACRO", "undefined macro: " + name,
                         call.token.original.span);
                output.push_back(call.token);
                continue;
            }
            if (std::find(call.disabled.begin(), call.disabled.end(), name) != call.disabled.end()) {
                diagnose(call.token.context, "PP_MACRO_RECURSION", "recursive macro: " + name,
                         call.token.original.span);
                output.push_back(call.token);
                continue;
            }
            const auto& macro = found->second;
            std::vector<std::vector<Work>> actuals;
            if (macro.function_like && !arguments(queue, call, macro, actuals)) {
                output.push_back(call.token);
                continue;
            }
            std::vector<std::vector<MacroToken>> expanded_args;
            for (auto& actual : actuals) {
                auto tokens = expand(std::deque<Work>(std::make_move_iterator(actual.begin()),
                                                      std::make_move_iterator(actual.end())), depth + 1);
                trim(tokens);
                expanded_args.push_back(std::move(tokens));
            }
            const auto context = result_.contexts.size();
            result_.contexts.push_back({macro.definition.file, call.token.context,
                                        call.token.original.span, ExpansionKind::Macro, name,
                                        macro.definition, std::nullopt, macro.definition_context});
            auto disabled = std::move(call.disabled);
            disabled.push_back(name);
            std::vector<Work> replacement;
            for (const auto& body : macro.body) {
                const auto parameter = word(body.original.kind) ?
                    std::find(macro.parameters.begin(), macro.parameters.end(), text(body)) :
                    macro.parameters.end();
                if (parameter == macro.parameters.end()) {
                    replacement.push_back({{body.original, context}, disabled});
                    continue;
                }
                const auto index = static_cast<std::size_t>(parameter - macro.parameters.begin());
                std::map<std::size_t, std::size_t> origins;
                for (const auto& argument : expanded_args[index]) {
                    auto [origin, inserted] = origins.emplace(argument.context, result_.contexts.size());
                    if (inserted) {
                        result_.contexts.push_back({argument.original.span.file, context,
                            argument.original.span, ExpansionKind::Argument, name,
                            body.original.span, argument.context, body.context});
                    }
                    replacement.push_back({{argument.original, origin->second}, disabled});
                }
            }
            for (auto i = replacement.rbegin(); i != replacement.rend(); ++i) {
                queue.push_front(std::move(*i));
            }
        }
        return output;
    }

    std::vector<MacroToken> expand(const std::vector<MacroToken>& tokens) {
        std::deque<Work> queue;
        for (const auto& token : tokens) {
            queue.push_back({token, {}});
        }
        return expand(std::move(queue));
    }

    void change_condition(std::vector<Condition>& stack, const MacroToken& directive,
                          const std::vector<MacroToken>& operands) {
        const auto name = text(directive);
        const auto args = significant(operands);
        const bool takes_name = name == "`ifdef" || name == "`ifndef" || name == "`elsif";
        bool defined = false;
        if (takes_name) {
            if (args.size() != 1 || !word(args[0].original.kind)) {
                diagnose(directive.context, "PP_CONDITION_NAME",
                         "conditional directive requires one macro name", directive.original.span);
            } else {
                defined = macros_.contains(std::string(text(args[0])));
            }
        } else if (!args.empty()) {
            diagnose(directive.context, "PP_DIRECTIVE_ARGS", "unexpected directive operands",
                     directive.original.span);
        }
        if (name == "`ifdef" || name == "`ifndef") {
            const bool parent = stack.empty() || stack.back().active;
            const bool selected = name == "`ifdef" ? defined : !defined;
            stack.push_back({parent, parent && selected, selected, false, directive.original.span});
            return;
        }
        if (stack.empty()) {
            diagnose(directive.context, "PP_CONDITION_STACK", "conditional directive without ifdef/ifndef",
                     directive.original.span);
            return;
        }
        auto& frame = stack.back();
        if (name == "`endif") {
            stack.pop_back();
        } else if (frame.seen_else) {
            diagnose(directive.context, "PP_CONDITION_STACK", "duplicate else or elsif after else",
                     directive.original.span);
            frame.active = false;
        } else if (name == "`else") {
            frame.active = frame.parent_active && !frame.taken;
            frame.taken = true;
            frame.seen_else = true;
        } else {
            frame.active = frame.parent_active && !frame.taken && defined;
            frame.taken = frame.taken || defined;
        }
    }

    // 用宏表内容而非“文件是否已在栈上”检测包含环。
    // 有保护宏的自包含第一次会改变宏表，第二次读取时可正常跳过。
    std::string macro_state() const {
        std::string state;
        const auto add = [&state](std::string_view part) {
            state += std::to_string(part.size()) + ':' + std::string(part);
        };
        for (const auto& [name, macro] : macros_) {
            add(name);
            add(macro.function_like ? "function" : "object");
            add(std::to_string(macro.parameters.size()));
            for (const auto& parameter : macro.parameters) {
                add(parameter);
            }
            add("body");
            add(std::to_string(macro.body.size()));
            for (const auto& token : macro.body) {
                add(text(token));
            }
            add("end");
        }
        return state;
    }

    void include(const std::vector<MacroToken>& operands, const MacroToken& directive) {
        const auto args = significant(expand(operands));
        if (args.size() != 1 || args[0].original.kind != TokenKind::StringLiteral) {
            diagnose(directive.context, "PP_INCLUDE_PATH", "include requires one quoted file name",
                     directive.original.span);
            return;
        }
        try {
            const auto file = result_.files.at(directive.original.span.file.value);
            const auto target = resolve_include(sources_, *file, text(args[0]), options_.include_dirs);
            const auto context = result_.contexts.size();
            result_.contexts.push_back({target, directive.context, directive.original.span,
                                        ExpansionKind::Include});
            process_file(target, context);
        } catch (const std::exception& error) {
            diagnose(directive.context, "PP_INCLUDE_LOAD", error.what(), directive.original.span);
        }
    }

    void process_file(FileId id, std::size_t context) {
        const auto file = sources_.get(id);
        result_.files.emplace(id.value, file);
        const auto state = std::make_pair(id.value, macro_state());
        if (std::find(active_files_.begin(), active_files_.end(), state) != active_files_.end()) {
            diagnose(context, "PP_INCLUDE_CYCLE", "include cycle without a changing macro state",
                     {id, 0, 0});
            return;
        }
        if (active_files_.size() >= 128) {
            diagnose(context, "PP_INCLUDE_DEPTH", "include nesting exceeds 128", {id, 0, 0});
            return;
        }
        active_files_.push_back(state);
        const auto lexed = lex(file);
        std::vector<Condition> conditions;
        std::vector<MacroToken> chunk;
        const auto flush = [&] {
            for (const auto& token : expand(chunk)) {
                append(token);
            }
            chunk.clear();
        };
        const auto active = [&] { return conditions.empty() || conditions.back().active; };
        const auto lexical_errors = [&](std::size_t begin, std::size_t end) {
            for (const auto& error : lexed.diagnostics) {
                if (error.primary && error.primary->begin >= begin && error.primary->begin < end) {
                    diagnose(context, error.code, error.message, *error.primary);
                }
            }
        };
        for (std::size_t i = 0; i < lexed.tokens.size(); ++i) {
            const auto& token = lexed.tokens[i];
            if (token.kind == TokenKind::EndOfFile) {
                break;
            }
            if (token.kind == TokenKind::Directive && source_directive(lexed.text(token))) {
                flush();
                auto end = i + 1;
                std::vector<MacroToken> operands;
                while (end < lexed.tokens.size() &&
                       lexed.tokens[end].kind != TokenKind::Newline &&
                       lexed.tokens[end].kind != TokenKind::EndOfFile) {
                    operands.push_back({lexed.tokens[end++], context});
                }
                const auto name = lexed.text(token);
                const MacroToken directive{token, context};
                if (active()) {
                    lexical_errors(token.span.begin, lexed.tokens[end].span.begin);
                }
                if (conditional(name)) {
                    change_condition(conditions, directive, operands);
                } else if (active()) {
                    if (name == "`define") {
                        define_macro(operands, directive);
                    } else if (name == "`include") {
                        include(operands, directive);
                    } else if (name == "`undefineall") {
                        if (!significant(operands).empty()) {
                            diagnose(context, "PP_DIRECTIVE_ARGS", "undefineall takes no operands",
                                     token.span);
                        }
                        macros_.clear();
                    } else {
                        const auto args = significant(operands);
                        if (args.size() != 1 || !word(args[0].original.kind)) {
                            diagnose(context, "PP_UNDEF_NAME", "undef requires one macro name", token.span);
                        } else {
                            macros_.erase(std::string(text(args[0])));
                        }
                    }
                }
                // 保留换行作为分界，但定义、指令与未选中分支不送进 Parser。
                if (lexed.tokens[end].kind == TokenKind::Newline) {
                    append({lexed.tokens[end], context});
                }
                i = end - (lexed.tokens[end].kind == TokenKind::EndOfFile ? 1 : 0);
            } else if (active()) {
                if (token.kind == TokenKind::Error) {
                    lexical_errors(token.span.begin, token.span.end);
                }
                if (token.kind != TokenKind::LineContinuation &&
                    (context == 0 || token.kind != TokenKind::ByteOrderMark)) {
                    chunk.push_back({token, context});
                }
            }
        }
        flush();
        for (const auto& condition : conditions) {
            diagnose(context, "PP_UNTERMINATED_CONDITION", "missing endif", condition.opening);
        }
        active_files_.pop_back();
    }

    SourceManager& sources_;
    const PreprocessOptions& options_;
    PreprocessResult result_;
    MacroTable macros_;
    std::vector<std::pair<std::uint32_t, std::string>> active_files_;
    std::optional<FileId> spacing_;
    std::size_t expanded_end_ = 0;
    std::size_t work_count_ = 0;
    bool limit_reported_ = false;
};

} // namespace

PreprocessResult preprocess(SourceManager& sources, FileId entry, const PreprocessOptions& options) {
    return Preprocessor(sources, options).run(entry);
}

} // namespace vagrad
