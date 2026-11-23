#include "vagrad/frontend/lexer/lexer.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

vagrad::LexResult scan(std::string text) {
    return vagrad::lex(std::make_shared<const vagrad::SourceFile>(
        vagrad::FileId{0}, "test.va", std::move(text)));
}

void require_lossless(const vagrad::LexResult& result) {
    std::size_t cursor = 0;
    std::string reconstructed;
    require(!result.tokens.empty(), "EOF token is required");
    for (std::size_t i = 0; i < result.tokens.size(); ++i) {
        const auto& token = result.tokens[i];
        require(token.span.file == result.source->id(), "token FileId");
        require(token.span.begin == cursor, "token coverage has a gap or overlap");
        if (token.kind == vagrad::TokenKind::EndOfFile) {
            require(i + 1 == result.tokens.size() && token.span.end == cursor,
                    "EOF must be the last zero-width token");
        } else {
            require(token.span.end > token.span.begin, "scanner must advance");
        }
        reconstructed += result.text(token);
        cursor = token.span.end;
    }
    require(cursor == result.source->text().size(), "token coverage reaches EOF");
    require(reconstructed == result.source->text(), "token text reconstructs every input byte");
}

std::vector<std::pair<vagrad::TokenKind, std::string>> significant(
    const vagrad::LexResult& result) {
    std::vector<std::pair<vagrad::TokenKind, std::string>> tokens;
    for (const auto& token : result.tokens) {
        if (!vagrad::is_trivia(token.kind) && token.kind != vagrad::TokenKind::EndOfFile) {
            tokens.emplace_back(token.kind, result.text(token));
        }
    }
    return tokens;
}

void check_classes() {
    using K = vagrad::TokenKind;
    const auto result = scan("module modulex x$1 $temperature \\escaped.name \n"
                             "`include \"file.vams\" I(p,n) <+ 1e-3;");
    require_lossless(result);
    require(!result.has_errors(), "normal input");
    const auto tokens = significant(result);
    const std::vector<std::pair<K, std::string>> expected = {
        {K::Keyword, "module"}, {K::Identifier, "modulex"}, {K::Identifier, "x$1"},
        {K::SystemIdentifier, "$temperature"}, {K::EscapedIdentifier, "\\escaped.name"},
        {K::Directive, "`include"}, {K::StringLiteral, "\"file.vams\""},
        {K::Identifier, "I"}, {K::Symbol, "("}, {K::Identifier, "p"}, {K::Symbol, ","},
        {K::Identifier, "n"}, {K::Symbol, ")"}, {K::Symbol, "<+"},
        {K::NumberLiteral, "1e-3"}, {K::Symbol, ";"}
    };
    require(tokens == expected, "token kinds and spellings");

    const auto ops = scan("<<< >>> === !== <+ < >= > ** * && & || | (* *) '{ +: -: ++ -- ::");
    require_lossless(ops);
    const auto spellings = significant(ops);
    require(spellings.size() == 22 && spellings[0].second == "<<<" &&
            spellings[4].second == "<+" && spellings[16].second == "'{",
            "longest operator match");
}

void check_trivia() {
    const std::string text = "\xef\xbb\xbf// \xe7\x94\xb5\xe6\xb5\x81\r\n"
                             "\t/* block\n comment */ \\\r\nreal x;\r";
    const auto result = scan(text);
    require_lossless(result);
    require(!result.has_errors(), "comments, BOM and continuation");
    require(result.tokens.front().kind == vagrad::TokenKind::ByteOrderMark, "BOM token");
    bool continued = false;
    for (const auto& token : result.tokens) {
        if (token.kind == vagrad::TokenKind::LineContinuation) {
            continued = result.text(token) == "\\\r\n";
        }
    }
    require(continued, "CRLF continuation retained");
    const auto end = result.source->locate(result.tokens.back().span.begin);
    require(end.line == 5 && end.column == 1, "token EOF location");

    const auto quoted = scan("\"a\\\"b\\\\c\\n\" \"continued\\\r\ntext\"");
    require_lossless(quoted);
    require(!quoted.has_errors() && significant(quoted).size() == 2, "escaped strings");

    std::ostringstream printed;
    vagrad::print_tokens(printed, scan(" \t\r\n"));
    require(printed.str().find("Whitespace \" \\t\"") != std::string::npos &&
            printed.str().find("Newline \"\\r\\n\"") != std::string::npos,
            "dump escapes whitespace for one-token-per-line output");
}

void check_numbers() {
    const auto result = scan("0 1_000 0.5 .5 1.0e-3 1E+4 1u 2.5n "
                             "8'hfF 4'sb10xz 'd42 '1 '0 'x");
    require_lossless(result);
    require(!result.has_errors(), "valid numeric forms");
    for (const auto& token : significant(result)) {
        require(token.first == vagrad::TokenKind::NumberLiteral, "number token kind");
    }

    const auto bad = scan("1e+; 4'b102; real recovered;");
    require_lossless(bad);
    require(bad.diagnostics.size() == 2 && bad.has_errors(), "numeric diagnostics");
    require(bad.diagnostics[0].code == "LEX_NUMBER_EXPONENT" &&
            bad.diagnostics[1].code == "LEX_NUMBER_DIGITS", "specific numeric error codes");
    require(significant(bad).back().second == ";", "lexing continues after errors");
}

void check_errors_and_boundaries() {
    for (const auto& input : {
             std::string{}, std::string{"/* comment"}, std::string{"\"bad\nreal x;"},
             std::string{"` "}, std::string{"\\"}, std::string{"a\0b", 3}}) {
        const auto result = scan(input);
        require_lossless(result);
        require(input.empty() || result.has_errors(), "invalid source is diagnosed");
    }
    const auto unterminated = scan("\"bad\nreal recovered;");
    bool recovered = false;
    for (const auto& token : unterminated.tokens) {
        recovered = recovered || (token.kind == vagrad::TokenKind::Keyword &&
                                  unterminated.text(token) == "real");
    }
    require(recovered, "unterminated string recovers at newline");

    // 检查所有单字节输入，避免尾部字节或 NUL 造成越界、丢失或停滞。
    for (int byte = 0; byte < 256; ++byte) {
        require_lossless(scan(std::string(1, static_cast<char>(byte))));
    }
}

void check_fixture() {
    using K = vagrad::TokenKind;
    vagrad::SourceManager sources;
    const auto id = sources.load(std::filesystem::path(VAGRAD_TEST_DATA_DIR) / "simple_mos.va");
    const auto result = vagrad::lex(sources.get(id));
    require_lossless(result);
    require(!result.has_errors(), "simple_mos fixture");
    const auto tokens = significant(result);
    require(tokens.front().second == "module", "module token");
    const auto has = [&](K kind, const std::string& text) {
        return std::find(tokens.begin(), tokens.end(), std::pair{kind, text}) != tokens.end();
    };
    require(has(K::Directive, "`include") &&
            has(K::StringLiteral, "\"include/simple_mos_defaults.vams\""),
            "relative include directive and file name");
    require(has(K::Directive, "`ifdef") &&
            has(K::Directive, "`else") && has(K::Directive, "`endif"),
            "conditional compilation remains visible in the raw stream");
    require(has(K::Directive, "`POSITIVE") && has(K::Directive, "`OVERDRIVE") &&
            has(K::Directive, "`SQUARE"), "macro references remain unexpanded");
    require(has(K::SystemIdentifier, "$temperature") && has(K::SystemIdentifier, "$strobe"),
            "system names in the model");
    for (const auto* number : {"1u", "1e-12", "3.0e2", ".5", "8'h0F"}) {
        require(has(K::NumberLiteral, number), std::string("model numeric form: ") + number);
    }
    for (const auto* op : {"&&", "!=", "<+", "(*", "*)"}) {
        require(has(K::Symbol, op), std::string("model operator: ") + op);
    }

    bool comment_contains_fake_code = false;
    bool continued_macro = false;
    bool escaped_string = false;
    std::size_t defines = 0;
    for (const auto& token : result.tokens) {
        const auto text = result.text(token);
        comment_contains_fake_code = comment_contains_fake_code ||
            (token.kind == K::BlockComment && text.find("`define COMMENT_ONLY") != text.npos);
        continued_macro = continued_macro || token.kind == K::LineContinuation;
        escaped_string = escaped_string ||
            (token.kind == K::StringLiteral && text.find("\\\"simple MOS\\\"") != text.npos);
        if (token.kind == K::Directive && text == "`define") {
            ++defines;
        }
    }
    require(comment_contains_fake_code && defines == 4,
            "macro-looking text inside a comment is a comment");
    require(continued_macro && escaped_string, "macro continuation and string escapes");

    const auto header_id = sources.load(result.source->path().parent_path() /
                                         "include/simple_mos_defaults.vams");
    const auto header = vagrad::lex(sources.get(header_id));
    require_lossless(header);
    require(!header.has_errors() && header_id != id, "include file has its own source identity");
    const auto header_tokens = significant(header);
    for (const auto& expected : std::vector<std::pair<K, std::string>>{
             {K::Directive, "`ifndef"}, {K::Identifier, "SIMPLE_MOS_DEFAULTS_VAMS"},
             {K::Identifier, "DEFAULT_VTH"}, {K::NumberLiteral, "0.45"},
             {K::Identifier, "DEFAULT_GAIN"}, {K::NumberLiteral, "1m"},
             {K::Directive, "`endif"}}) {
        require(std::find(header_tokens.begin(), header_tokens.end(), expected) != header_tokens.end(),
                "include guard and default parameter tokens");
    }
}

} // namespace

int main() {
    try {
        check_classes();
        check_trivia();
        check_numbers();
        check_errors_and_boundaries();
        check_fixture();
        std::cout << "lossless token coverage, lexical classes, positions and recovery passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

