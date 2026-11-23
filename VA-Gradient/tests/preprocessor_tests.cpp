#include "vagrad/frontend/preprocessor/preprocessor.hpp"
#include "vagrad/frontend/lexer/lexer.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace vagrad;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Fixture {
public:
    Fixture() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::current_path() / ("preprocessor-test-" + std::to_string(stamp));
        require(fs::create_directory(root), "create test directory");
        fs::create_directory(root / "inc");
    }
    ~Fixture() {
        std::error_code ignored;
        for (const auto& file : files_) {
            fs::remove(file, ignored);
        }
        fs::remove(root / "inc", ignored);
        fs::remove(root, ignored);
    }
    fs::path write(const fs::path& name, std::string_view content) {
        const auto path = root / name;
        std::ofstream output(path, std::ios::binary);
        output << content;
        output.close();
        require(static_cast<bool>(output), "write test fixture");
        files_.push_back(path);
        return path;
    }
    fs::path root;
private:
    std::vector<fs::path> files_;
};

std::string_view spelling(const PreprocessResult& result, const Token& token) {
    return result.files.at(token.span.file.value)->slice(token.span.begin, token.span.end);
}

PreprocessResult from_text(std::string text, const PreprocessOptions& options = {}) {
    SourceManager sources;
    const auto id = sources.add_virtual("<test.va>", std::move(text));
    return preprocess(sources, id, options);
}

std::string rendered(const PreprocessResult& result) {
    std::ostringstream out;
    print_preprocessed_source(out, result);
    return out.str();
}

std::string significant_text(const PreprocessResult& result) {
    std::string text;
    for (const auto& token : result.tokens) {
        if (!is_trivia(token.original.kind) && token.original.kind != TokenKind::EndOfFile) {
            text += std::string(spelling(result, token.original)) + ' ';
        }
    }
    return text;
}

void check_stream(const PreprocessResult& result) {
    std::size_t cursor = 0;
    std::size_t eof = 0;
    std::string joined;
    for (const auto& item : result.tokens) {
        require(item.begin == cursor, "continuous expanded range");
        require(result.contexts.at(item.context).file == item.original.span.file,
                "token context matches original file");
        const auto text = spelling(result, item.original);
        require(item.end - item.begin == text.size(), "expanded token size");
        cursor = item.end;
        joined += text;
        eof += item.original.kind == TokenKind::EndOfFile ? 1 : 0;
    }
    require(eof == 1 && result.tokens.back().original.kind == TokenKind::EndOfFile, "one final EOF");
    require(rendered(result) == joined, "printed text matches expanded offsets");
    for (std::size_t i = 0; i < result.contexts.size(); ++i) {
        const auto& origin = result.contexts[i];
        require(result.files.contains(origin.file.value), "context owns its source snapshot");
        if (origin.parent) {
            require(*origin.parent < i && origin.call_site.has_value(), "acyclic parent contexts");
        }
        if (origin.argument_origin) {
            require(*origin.argument_origin < i, "argument origin points backwards");
        }
        if (origin.definition_origin) {
            require(*origin.definition_origin < i, "definition origin points backwards");
        }
    }
    if (result.has_errors()) {
        return;
    }
    // 文本导出再词法分析，与展开 token 的有效记号序列逐个一致。
    const auto relexed = lex(std::make_shared<SourceFile>(FileId{}, "<expanded>", joined));
    require(!relexed.has_errors(), "exported source is lexically valid");
    std::vector<std::pair<TokenKind, std::string>> expected, actual;
    for (const auto& item : result.tokens) {
        if (!is_trivia(item.original.kind)) {
            expected.emplace_back(item.original.kind, spelling(result, item.original));
        }
    }
    for (const auto& item : relexed.tokens) {
        if (!is_trivia(item.kind)) {
            actual.emplace_back(item.kind, relexed.text(item));
        }
    }
    require(expected == actual, "text export preserves token boundaries");
}

bool has_code(const PreprocessResult& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& diagnostic) { return diagnostic.code == code; });
}

void macros() {
    const auto result = from_text(
        "`define ID(x) x\n"
        "`define ALIAS `ID\n"
        "`define INC(x) ((x) + 1)\n"
        "`define DUP(x) x + x\n"
        "`define ZERO() 7\n"
        "`define MULTI(x) \\\n  ((x) * (x))\n"
        "a = `ALIAS(`ID(2));\n"
        "b = `DUP(`INC(f(1, 2)));\n"
        "c = `ZERO(); d = `MULTI(3); e = `ID();\n"
        "// `MISSING is a comment\n"
        "s = \"`ID(4)\";\n");
    require(!result.has_errors(), "nested macros, alias, empty argument and multiline definition");
    const auto text = significant_text(result);
    require(text.find("a = 2 ;") != text.npos, "alias consumes following invocation");
    require(text.find("c = 7 ;") != text.npos, "zero formal parameters");
    require(text.find("d = ( ( 3 ) * ( 3 ) ) ;") != text.npos, "line continuation removed");
    require(text.find("e = ;") != text.npos, "empty actual argument");
    require(text.find("\"`ID(4)\"") != text.npos, "string is not substituted");
    require(rendered(result).find("// `MISSING") != text.npos, "comment is not expanded");
    require(std::count_if(result.contexts.begin(), result.contexts.end(),
        [](const auto& item) { return item.kind == ExpansionKind::Argument; }) >= 6,
        "each argument occurrence has provenance");
    check_stream(result);

    const auto boundaries = from_text(
        "`define PLUS +\n"
        "`define EMPTY\n"
        "`define NAME abc\n"
        "a = 1 `PLUS+ 2;\n"
        "b = `NAME`EMPTY`NAME;\n");
    require(!boundaries.has_errors(), "adjacent replacements");
    require(significant_text(boundaries).find("abc abc") != std::string::npos,
            "separate macro tokens must not fuse into one identifier");
    check_stream(boundaries);
}

void conditions() {
    const auto result = from_text(
        "`define FLAG 0\n"
        "`ifdef FLAG\n"
        "real yes;\n"
        "`ifdef MISSING\n"
        "`include \"does-not-exist.vams\"\n"
        "`UNKNOWN\n"
        "`else\n"
        "real inner;\n"
        "`endif\n"
        "`elsif FLAG\n"
        "real no;\n"
        "`else\n"
        "real also_no;\n"
        "`endif\n"
        "`undef FLAG\n"
        "`ifdef FLAG\n"
        "bad\n"
        "`elsif MISSING\n"
        "bad_again\n"
        "`else\n"
        "real fallback;\n"
        "`endif\n"
        "`define VALUE 1\n"
        "a = `VALUE;\n"
        "`define VALUE 2\n"
        "b = `VALUE;\n"
        "if (a > b) a = b;\n");
    require(!result.has_errors(), "nested conditions, inactive includes and undef");
    const auto text = significant_text(result);
    require(text.find("real yes ; real inner ; real fallback ; a = 1 ; b = 2 ;") != text.npos,
            "definition presence, branch selection and source order");
    require(text.find("if ( a > b )") != text.npos, "ordinary if remains for the Parser");
    require(result.files.size() == 1, "inactive includes are not opened");
    check_stream(result);

    PreprocessOptions options;
    options.defines = {{"SECOND", "0"}, {"VALUE", "2.5"}};
    const auto command = from_text(
        "`ifdef FIRST\nwrong\n`elsif SECOND\nx = `VALUE;\n`else\nwrong\n`endif\n", options);
    require(!command.has_errors() && significant_text(command) == "x = 2.5 ; ",
            "command-line macros and elsif");
    check_stream(command);
}

void includes() {
    Fixture fixture;
    const auto main = fixture.write("main.va",
        "`define HEADER \"guard.vams\"\n"
        "`include `HEADER\n`include \"guard.vams\"\ny = `GAIN;\n");
    fixture.write("inc/guard.vams",
        "`ifndef GUARD\n`define GUARD 1\n"
        "`include \"guard.vams\"\n"
        "`define GAIN 1m\nreal once;\n`endif\n");
    SourceManager sources;
    PreprocessOptions options;
    options.include_dirs.push_back(fixture.root / "inc");
    const auto result = preprocess(sources, sources.load(main), options);
    require(!result.has_errors(), "search paths, macro include name and guarded self include");
    require(significant_text(result) == "real once ; y = 1m ; ", "include guard emits body once");
    require(result.files.size() == 2, "same header has one snapshot");
    bool header_macro = false;
    for (const auto& item : result.tokens) {
        if (spelling(result, item.original) == "1m") {
            const auto& origin = result.contexts[item.context];
            require(origin.kind == ExpansionKind::Macro && origin.parent == 0,
                    "macro call belongs to main file");
            require(origin.definition && origin.definition->file == item.original.span.file &&
                    item.original.span.file.value != 0, "macro definition belongs to header");
            require(origin.definition_origin &&
                    result.contexts[*origin.definition_origin].kind == ExpansionKind::Include,
                    "macro definition remembers the particular header inclusion");
            header_macro = true;
        }
    }
    require(header_macro, "header macro provenance checked");
    check_stream(result);

    const auto bad = fixture.write("cycle.va", "`include \"cycle.va\"\n");
    SourceManager cyclic;
    const auto cycle = preprocess(cyclic, cyclic.load(bad));
    require(has_code(cycle, "PP_INCLUDE_CYCLE"), "unguarded include cycle terminates");
    require(!cycle.diagnostics[0].related.empty(), "include stack in cycle diagnostic");
    check_stream(cycle);
}

void nested_includes() {
    Fixture fixture;
    const auto main = fixture.write("main.va",
        "real before;\n`include \"inc/a.vams\"\nreal after;\n");
    fixture.write("inc/a.vams", "`include \"b.vams\"\nreal a;\n");
    fixture.write("inc/b.vams", "real b;\n// `include \"ignored.vams\"\n");
    SourceManager sources;
    const auto nested = preprocess(sources, sources.load(main));
    require(!nested.has_errors(), "nested relative includes");
    require(significant_text(nested) == "real before ; real b ; real a ; real after ; ",
            "included declarations retain their insertion order");
    require(nested.files.size() == 3 && nested.contexts.size() == 3 &&
            nested.contexts[1].parent == 0 && nested.contexts[2].parent == 1,
            "nested include contexts");
    check_stream(nested);

    const auto repeated = fixture.write("repeated.va",
        "`include \"inc/b.vams\"\n`include \"inc/b.vams\"\n");
    SourceManager repeated_sources;
    const auto twice = preprocess(repeated_sources, repeated_sources.load(repeated));
    require(!twice.has_errors() && twice.files.size() == 2 && twice.contexts.size() == 3,
            "repeated includes share snapshots but not inclusion contexts");
    require(significant_text(twice) == "real b ; real b ; ",
            "an unguarded header contributes declarations on each inclusion");
    check_stream(twice);

    const auto missing_main = fixture.write("missing-main.va", "`include \"inc/missing.vams\"\n");
    fixture.write("inc/missing.vams", "`include \"absent.vams\"\nreal recovered;\n");
    SourceManager missing_sources;
    const auto missing = preprocess(missing_sources, missing_sources.load(missing_main));
    require(has_code(missing, "PP_INCLUDE_LOAD") && missing.diagnostics[0].related.size() == 1,
            "missing nested header reports the inclusion chain");
    require(significant_text(missing) == "real recovered ; ", "recover after a failed include");
    check_stream(missing);

    const auto malformed = from_text("`include FILENAME\nreal recovered;\n");
    require(has_code(malformed, "PP_INCLUDE_PATH") &&
            significant_text(malformed) == "real recovered ; ", "recover after an invalid include");
    check_stream(malformed);
}

void errors() {
    const std::vector<std::pair<std::string, std::string>> cases{
        {"x = `MISSING;\n", "PP_UNDEFINED_MACRO"},
        {"`define A `B\n`define B `A\nx = `A;\n", "PP_MACRO_RECURSION"},
        {"`define F(x) x\nx = `F(1, 2);\n", "PP_MACRO_ARITY"},
        {"`define F(x) x\nx = `F;\n", "PP_MACRO_ARGS"},
        {"`define F(x) x\nx = `F(1;\n", "PP_MACRO_ARGS"},
        {"`define F(x, x) x\n", "PP_DEFINE_PARAMS"},
        {"`define F(x x\n", "PP_DEFINE_PARAMS"},
        {"`define 12 3\n", "PP_DEFINE_NAME"},
        {"`ifdef FLAG\nreal x;\n", "PP_UNTERMINATED_CONDITION"},
        {"`else\n", "PP_CONDITION_STACK"},
        {"`ifdef X\n`else\n`else\n`endif\n", "PP_CONDITION_STACK"},
        {"`include 1\n", "PP_INCLUDE_PATH"},
        {"`include \"missing.vams\"\n", "PP_INCLUDE_LOAD"},
        {"`ifdef X Y\n`endif\n", "PP_CONDITION_NAME"},
        {"`undef\n", "PP_UNDEF_NAME"},
        {"real x = \"unterminated\n", "LEX_STRING"}
    };
    for (const auto& [source, code] : cases) {
        const auto result = from_text(source);
        require(result.has_errors() && has_code(result, code), "expected diagnostic " + code);
        check_stream(result);
    }
    PreprocessOptions invalid;
    invalid.defines["bad-name"] = "2";
    require(has_code(from_text("x;\n", invalid), "PP_DEFINE_NAME"), "invalid -D macro name");
    const auto inactive = from_text("`ifdef MISSING\n\"unterminated\n`endif\nreal ok;\n");
    require(!inactive.has_errors(), "inactive branch does not report ordinary lexical errors");
    check_stream(inactive);
}

void model() {
    PreprocessResult result;
    {
        SourceManager sources;
        const auto input = fs::path(VAGRAD_TEST_DATA_DIR) / "simple_mos.va";
        result = preprocess(sources, sources.load(input));
    }
    require(!result.has_errors(), "default model preprocessing succeeds");
    const auto text = significant_text(result);
    require(text.find("parameter real G0 = 1m from") != text.npos, "header gain macro");
    require(text.find("parameter real VTH0 = 0.45 from") != text.npos, "header threshold macro");
    require(text.find("max") != text.npos && text.find("sqrt") == text.npos &&
            text.find("$strobe") == text.npos, "default conditional selection");
    require(std::none_of(result.tokens.begin(), result.tokens.end(),
        [](const auto& item) { return item.original.kind == TokenKind::Directive; }),
        "no unexpanded directives in valid model");
    bool argument_mapping = false;
    for (const auto& item : result.tokens) {
        if (spelling(result, item.original) == "vgs" &&
            result.contexts[item.context].kind == ExpansionKind::Argument) {
            require(item.original.span.file.value == 0, "argument remains in main file");
            require(result.contexts[item.context].argument_origin.has_value(), "argument graph edge");
            argument_mapping = true;
        }
    }
    require(argument_mapping, "nested argument provenance survives manager lifetime");
    check_stream(result);

    SourceManager sources;
    PreprocessOptions options;
    options.defines = {{"SOFT_THRESHOLD", "1"}, {"TRACE_MOS", "1"}};
    const auto soft = preprocess(sources,
        sources.load(fs::path(VAGRAD_TEST_DATA_DIR) / "simple_mos.va"), options);
    const auto soft_text = significant_text(soft);
    require(!soft.has_errors() && soft_text.find("sqrt") != soft_text.npos &&
            soft_text.find("$strobe") != soft_text.npos && soft_text.find("max") == soft_text.npos,
            "alternate model configuration");
    check_stream(soft);
}

} // namespace

int main() {
    try {
        macros();
        conditions();
        includes();
        nested_includes();
        errors();
        model();
        std::cout << "macros, conditionals, includes, source mapping and model checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
