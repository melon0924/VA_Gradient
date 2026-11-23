#include "vagrad/frontend/lexer/lexer.hpp"

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace vagrad {
namespace {

// 使用固定的 ASCII 规则，避免进程区域设置改变词法结果。
bool digit(char ch) { return ch >= '0' && ch <= '9'; }
bool letter(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }
bool name_start(char ch) { return letter(ch) || ch == '_'; }
bool name_part(char ch) { return name_start(ch) || digit(ch) || ch == '$'; }
bool horizontal_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\f' || ch == '\v'; }
bool newline(char ch) { return ch == '\r' || ch == '\n'; }

char lower(char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
}

bool keyword(std::string_view text) {
    constexpr std::string_view words[] = {
        "module", "endmodule", "macromodule", "nature", "endnature", "discipline",
        "enddiscipline", "analog", "initial", "function", "endfunction", "begin", "end",
        "input", "output", "inout", "parameter", "localparam", "aliasparam", "real",
        "integer", "string", "genvar", "time", "realtime", "reg", "wire", "wreal",
        "signed", "unsigned", "ground", "branch", "if", "else", "for", "while",
        "repeat", "forever", "case", "casex", "casez", "endcase", "default", "from",
        "exclude", "inf", "generate", "endgenerate", "or", "posedge", "negedge",
        "edge", "disable", "break", "continue", "return", "event", "connectmodule",
        "endconnectmodule"
    };
    return std::find(std::begin(words), std::end(words), text) != std::end(words);
}

class Lexer {
public:
    explicit Lexer(std::shared_ptr<const SourceFile> source)
        : result_{std::move(source), {}, {}}, input_(result_.source->text()) {}

    LexResult run() {
        while (position_ < input_.size()) {
            read_token();
        }
        emit(TokenKind::EndOfFile, position_);
        return std::move(result_);
    }

private:
    char peek(std::size_t distance = 0) const {
        return distance < input_.size() - position_ ? input_[position_ + distance] : '\0';
    }

    bool starts(std::string_view text) const {
        return input_.substr(position_, text.size()) == text;
    }

    bool accept(char ch) {
        if (position_ < input_.size() && peek() == ch) {
            ++position_;
            return true;
        }
        return false;
    }

    void consume_newline() {
        if (accept('\r')) {
            accept('\n');
        } else {
            accept('\n');
        }
    }

    void emit(TokenKind kind, std::size_t start) {
        result_.tokens.push_back({kind, {result_.source->id(), start, position_}});
    }

    void error(std::size_t start, std::string code, std::string message) {
        emit(TokenKind::Error, start);
        result_.diagnostics.push_back({
            Severity::Error, std::move(code), std::move(message),
            SourceSpan{result_.source->id(), start, position_}, {}
        });
    }

    void consume_digits() {
        while (digit(peek()) || peek() == '_') {
            ++position_;
        }
    }

    void based_number(std::size_t start) {
        if (peek() == 's' || peek() == 'S') {
            ++position_;
        }
        const auto base = std::string_view("bodh").find(lower(peek()));
        if (base == std::string_view::npos) {
            error(start, "LEX_NUMBER_BASE", "expected b, o, d or h after apostrophe");
            return;
        }
        ++position_;
        // 基数和数字之间的空白保留在本数字的原文范围中。
        while (horizontal_space(peek())) {
            ++position_;
        }
        constexpr std::string_view digits[] = {"01", "01234567", "0123456789",
                                               "0123456789abcdef"};
        bool valid = true;
        bool has_digit = false;
        while (name_part(peek()) || peek() == '?') {
            const char ch = lower(input_[position_++]);
            if (ch != '_') {
                has_digit = true;
                valid = valid && (digits[base].find(ch) != std::string_view::npos ||
                                  std::string_view("xz?").find(ch) != std::string_view::npos);
            }
        }
        if (!valid || !has_digit) {
            error(start, "LEX_NUMBER_DIGITS", "invalid or missing digits in based number");
            return;
        }
        emit(TokenKind::NumberLiteral, start);
    }

    void number(std::size_t start) {
        if (accept('\'')) {
            if (peek() != '\0' && std::string_view("01xXzZ").find(peek()) != std::string_view::npos) {
                ++position_;
                emit(TokenKind::NumberLiteral, start);
            } else {
                based_number(start);
            }
            return;
        }

        consume_digits();
        if (accept('.')) {
            consume_digits();
        }
        if (accept('\'')) {
            based_number(start);
            return;
        }
        if (accept('e') || accept('E')) {
            if (!accept('+')) {
                accept('-');
            }
            if (!digit(peek())) {
                error(start, "LEX_NUMBER_EXPONENT", "expected digits in exponent");
                return;
            }
            consume_digits();
        } else if (peek() != '\0' &&
                   std::string_view("TGMKkmunpfa").find(peek()) != std::string_view::npos) {
            ++position_;
        }
        emit(TokenKind::NumberLiteral, start);
    }

    void string_literal(std::size_t start) {
        ++position_;
        while (position_ < input_.size() && !newline(peek())) {
            if (accept('"')) {
                emit(TokenKind::StringLiteral, start);
                return;
            }
            if (accept('\\')) {
                if (newline(peek())) {
                    consume_newline();
                } else if (position_ < input_.size()) {
                    ++position_;
                }
            } else {
                ++position_;
            }
        }
        error(start, "LEX_STRING", "unterminated string literal");
    }

    void read_token() {
        const auto start = position_;
        if (position_ == 0 && starts("\xef\xbb\xbf")) {
            position_ += 3;
            emit(TokenKind::ByteOrderMark, start);
        } else if (horizontal_space(peek())) {
            do {
                ++position_;
            } while (horizontal_space(peek()));
            emit(TokenKind::Whitespace, start);
        } else if (newline(peek())) {
            consume_newline();
            emit(TokenKind::Newline, start);
        } else if (starts("//")) {
            position_ += 2;
            while (position_ < input_.size() && !newline(peek())) {
                ++position_;
            }
            emit(TokenKind::LineComment, start);
        } else if (starts("/*")) {
            position_ += 2;
            while (position_ < input_.size() && !starts("*/")) {
                ++position_;
            }
            if (starts("*/")) {
                position_ += 2;
                emit(TokenKind::BlockComment, start);
            } else {
                error(start, "LEX_COMMENT", "unterminated block comment");
            }
        } else if (name_start(peek()) || peek() == '$') {
            const bool system = peek() == '$';
            do {
                ++position_;
            } while (name_part(peek()));
            const auto text = input_.substr(start, position_ - start);
            const auto kind = system ? TokenKind::SystemIdentifier :
                              keyword(text) ? TokenKind::Keyword : TokenKind::Identifier;
            emit(kind, start);
        } else if (peek() == '\\') {
            ++position_;
            if (newline(peek())) {
                consume_newline();
                emit(TokenKind::LineContinuation, start);
                return;
            }
            while (position_ < input_.size() && !horizontal_space(peek()) && !newline(peek())) {
                ++position_;
            }
            if (position_ == start + 1) {
                error(start, "LEX_IDENTIFIER", "empty escaped identifier");
            } else {
                emit(TokenKind::EscapedIdentifier, start);
            }
        } else if (peek() == '`') {
            ++position_;
            if (!name_start(peek())) {
                error(start, "LEX_DIRECTIVE", "expected macro or directive name after backtick");
                return;
            }
            do {
                ++position_;
            } while (name_part(peek()));
            // 在词法层只识别引用的名字，展开由后续 Preprocessor 处理。
            emit(TokenKind::Directive, start);
        } else if (peek() == '"') {
            string_literal(start);
        } else if (digit(peek()) || (peek() == '.' && digit(peek(1))) ||
                   (peek() == '\'' && peek(1) != '{')) {
            number(start);
        } else {
            symbol(start);
        }
    }

    void symbol(std::size_t start) {
        // 长运算符优先，<+ 和 < 要保持不同的 token。
        constexpr std::string_view operators[] = {
            "<<<", ">>>", "===", "!==", "&&&", "<+", "**", "==", "!=", ">=", "<=",
            "&&", "||", "<<", ">>", "~&", "~|", "~^", "^~", "(*", "*)", "'{",
            "+:", "-:", "++", "--", "+=", "-=", "*=", "/=", "::", "%=", "&=", "|=", "^="
        };
        for (const auto op : operators) {
            if (starts(op)) {
                position_ += op.size();
                emit(TokenKind::Symbol, start);
                return;
            }
        }
        const char ch = input_[position_++];
        if (std::string_view("()[]{}.,:;#+-*/%&|^~!=<>?@").find(ch) != std::string_view::npos) {
            emit(TokenKind::Symbol, start);
        } else {
            error(start, "LEX_CHARACTER", "unexpected source byte");
        }
    }

    LexResult result_;
    std::string_view input_;
    std::size_t position_ = 0;
};

void print_escaped(std::ostream& out, std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char ch : text) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\r': out << "\\r"; break;
        case '\n': out << "\\n"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20 || ch == 0x7f) {
                out << "\\x" << hex[ch >> 4] << hex[ch & 0x0f];
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    out << '"';
}

} // namespace

std::string_view LexResult::text(const Token& token) const {
    if (token.span.file != source->id()) {
        throw std::invalid_argument("token refers to another source file");
    }
    return source->slice(token.span.begin, token.span.end);
}

bool LexResult::has_errors() const {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

LexResult lex(std::shared_ptr<const SourceFile> source) {
    if (!source) {
        throw std::invalid_argument("lexer requires a source snapshot");
    }
    return Lexer(std::move(source)).run();
}

void print_token(std::ostream& out, const SourceFile& source, const Token& token) {
    if (token.span.file != source.id()) {
        throw std::invalid_argument("token refers to another source file");
    }
    const auto location = source.locate(token.span.begin);
    out << location.line << ':' << location.column << " [" << token.span.begin << ','
        << token.span.end << ") " << token_kind_name(token.kind) << ' ';
    print_escaped(out, source.slice(token.span.begin, token.span.end));
}

void print_tokens(std::ostream& out, const LexResult& result) {
    for (const auto& token : result.tokens) {
        print_token(out, *result.source, token);
        out << '\n';
    }
}

} // namespace vagrad

