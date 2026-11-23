#include "vagrad/frontend/parser/parser.hpp"

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>

namespace vagrad {
namespace {

// 递归下降负责语句/声明，Pratt 负责表达式；二者写入同一事件列表。
class Parser {
public:
    explicit Parser(std::shared_ptr<const PreprocessResult> input) {
        if (!input || input->tokens.empty() ||
            input->tokens.back().original.kind != TokenKind::EndOfFile) {
            throw std::invalid_argument("Parser requires a preprocessed stream ending in EOF");
        }
        result_.input = std::move(input);
        for (std::size_t i = 0; i < result_.input->tokens.size(); ++i) {
            const auto kind = result_.input->tokens[i].original.kind;
            if (kind == TokenKind::EndOfFile && i + 1 != result_.input->tokens.size()) {
                throw std::invalid_argument("Parser requires exactly one final EOF");
            }
            if (!is_trivia(kind)) {
                significant_.push_back(i);
            }
        }
    }

    ParseResult run() {
        const auto root = start();
        while (!eof()) {
            const auto before = position_;
            const auto item = start();
            attributes();
            if (at("module") || at("macromodule")) {
                module(item);
            } else if (at("nature") || at("discipline")) {
                physical_type(item);
            } else {
                error("PARSE_ITEM", "expected module, nature or discipline");
                recover({";", "module", "macromodule", "nature", "discipline"});
                eat(";");
                complete(item, SyntaxKind::ErrorNode);
            }
            progress(before);
        }
        bump(); // EOF 事件同时让 TreeBuilder 收入文件末尾的 trivia。
        complete(root, SyntaxKind::Root);
        return std::move(result_);
    }

private:
    struct Marker { std::size_t event; };
    struct CompletedMarker { std::size_t event; };
    struct Depth {
        std::size_t& value;
        explicit Depth(std::size_t& depth) : value(depth) { ++value; }
        ~Depth() { --value; }
    };

    Marker start() {
        result_.events.push_back({ParseEventKind::StartNode});
        return {result_.events.size() - 1};
    }
    CompletedMarker complete(Marker marker, SyntaxKind kind) {
        result_.events[marker.event].node_kind = kind;
        result_.events.push_back({ParseEventKind::FinishNode});
        return {marker.event};
    }
    Marker precede(CompletedMarker child) {
        const auto parent = start();
        result_.events[child.event].forward_parent = parent.event - child.event;
        return parent;
    }

    std::size_t index(std::size_t lookahead = 0) const {
        return significant_[std::min(position_ + lookahead, significant_.size() - 1)];
    }
    const Token& token(std::size_t lookahead = 0) const {
        return result_.input->tokens[index(lookahead)].original;
    }
    std::string_view spelling(std::size_t lookahead = 0) const {
        const auto span = token(lookahead).span;
        return result_.input->files.at(span.file.value)->slice(span.begin, span.end);
    }
    bool at(std::string_view value) const { return spelling() == value; }
    bool one_of(std::initializer_list<std::string_view> values) const {
        return std::find(values.begin(), values.end(), spelling()) != values.end();
    }
    bool eof() const { return token().kind == TokenKind::EndOfFile; }
    bool name(std::size_t lookahead = 0) const {
        const auto kind = token(lookahead).kind;
        return kind == TokenKind::Identifier || kind == TokenKind::EscapedIdentifier ||
               kind == TokenKind::SystemIdentifier;
    }
    bool type() const {
        return one_of({"real", "integer", "string", "genvar", "time", "realtime",
                       "reg", "wire", "wreal", "signed", "unsigned"});
    }
    bool direction() const { return one_of({"input", "output", "inout"}); }
    bool declaration_start() const {
        return type() || direction() ||
               one_of({"parameter", "localparam", "aliasparam", "branch", "ground"}) ||
               (name() && name(1)); // discipline 名 + net 名，语义绑定交给 HIR
    }
    bool boundary() const {
        return eof() || one_of({";", ")", "]", "}", ",", ":", "end", "else", "endmodule",
                               "endcase", "endfunction", "endnature", "enddiscipline",
                               "from", "exclude", "*)"});
    }

    void bump() {
        ParseEvent event{ParseEventKind::Token};
        event.token_index = index();
        result_.events.push_back(event);
        if (position_ + 1 < significant_.size()) {
            ++position_;
        }
    }
    bool eat(std::string_view value) {
        if (!at(value)) {
            return false;
        }
        bump();
        return true;
    }
    bool expect(std::string_view value) {
        if (eat(value)) {
            return true;
        }
        error("PARSE_EXPECTED", "expected '" + std::string(value) + "'");
        return false;
    }
    void expect_name() {
        if (name()) {
            bump();
        } else {
            error("PARSE_NAME", "expected an identifier");
        }
    }
    void error(std::string code, std::string message) {
        const auto& current = result_.input->tokens[index()];
        Diagnostic diagnostic{Severity::Error, std::move(code), std::move(message),
                              current.original.span, {}};
        auto context = std::optional<std::size_t>{current.context};
        while (context) {
            const auto& origin = result_.input->contexts.at(*context);
            if (origin.call_site) {
                diagnostic.related.push_back({*origin.call_site,
                    origin.kind == ExpansionKind::Include ? "included from here" : "expanded from here"});
            }
            if (origin.definition) {
                diagnostic.related.push_back({*origin.definition, "macro definition/formal parameter"});
            }
            context = origin.parent;
        }
        ParseEvent event{ParseEventKind::Error};
        event.diagnostic_index = result_.diagnostics.size();
        result_.diagnostics.push_back(std::move(diagnostic));
        result_.events.push_back(event);
    }

    // 错误区段也必须通过 Token 事件进入树，不能只移动游标丢掉原文。
    void recover(std::initializer_list<std::string_view> stop) {
        if (eof() || one_of(stop)) {
            return;
        }
        const auto skipped = start();
        while (!eof() && !one_of(stop)) {
            bump();
        }
        complete(skipped, SyntaxKind::ErrorNode);
    }
    void progress(std::size_t before) {
        if (before == position_ && !eof()) {
            const auto bad = start();
            error("PARSE_UNEXPECTED", "unexpected token '" + std::string(spelling()) + "'");
            bump();
            complete(bad, SyntaxKind::ErrorNode);
        }
    }
    void semicolon() {
        if (eat(";")) {
            return;
        }
        error("PARSE_EXPECTED", "expected ';'");
        // 下一个声明/赋值可作为恢复点，避免漏一个分号就吞掉下一条语句。
        if (boundary() || declaration_start() ||
            one_of({"analog", "begin", "if", "for", "while", "case", "module"}) ||
            (name() && (spelling(1) == "=" || spelling(1) == "<+"))) {
            return;
        }
        recover({";", "end", "else", "endmodule", "endfunction", "endcase"});
        eat(";");
    }

    void attributes() {
        while (at("(*")) {
            const auto list = start();
            bump();
            while (!eof() && !at("*)") && !at(";")) {
                const auto before = position_;
                const auto attribute = start();
                expect_name();
                if (eat("=")) {
                    expression();
                }
                complete(attribute, SyntaxKind::Attribute);
                if (!eat(",")) {
                    break;
                }
                progress(before);
            }
            expect("*)");
            complete(list, SyntaxKind::AttributeList);
        }
    }

    void array_range() {
        const auto range = start();
        expect("[");
        expression();
        if (eat(":")) {
            expression();
        }
        expect("]");
        complete(range, SyntaxKind::ArrayRange);
    }
    void parameter_range() {
        const auto range = start();
        bump(); // from / exclude
        if (at("(") || at("[")) {
            bump();
            expression();
            expect(":");
            expression();
            // 参数区间允许 (a:b]、[a:b) 等混合端点。
            if (at(")") || at("]")) {
                bump();
            } else {
                error("PARSE_RANGE", "expected ')' or ']' after parameter interval");
            }
        } else {
            expression();
        }
        complete(range, SyntaxKind::ParameterRange);
    }
    void declarator(bool parameter) {
        const auto item = start();
        attributes();
        expect_name();
        while (at("[")) {
            array_range();
        }
        if (eat("=")) {
            expression();
        } else if (parameter) {
            error("PARSE_PARAMETER_VALUE", "parameter requires an initial value");
        }
        while (at("from") || at("exclude")) {
            parameter_range();
        }
        complete(item, SyntaxKind::Declarator);
    }

    void declaration(Marker item) {
        const bool parameter = one_of({"parameter", "localparam", "aliasparam"});
        SyntaxKind kind = parameter ? SyntaxKind::ParameterDecl :
            direction() ? SyntaxKind::PortDecl : type() ? SyntaxKind::VariableDecl : SyntaxKind::NetDecl;
        if (at("branch")) {
            bump();
            arguments();
            do {
                expect_name();
            } while (eat(","));
            semicolon();
            complete(item, SyntaxKind::BranchDecl);
            return;
        }
        bump(); // parameter / 类型 / 方向 / discipline / ground
        while (type()) {
            bump();
        }
        if (kind == SyntaxKind::PortDecl && name() && name(1)) {
            bump(); // 可选的 discipline 名
        }
        while (at("[")) {
            array_range();
        }
        do {
            declarator(parameter);
        } while (eat(","));
        semicolon();
        complete(item, kind);
    }

    void ports() {
        const auto list = start();
        expect("(");
        while (!eof() && !at(")") && !at(";")) {
            const auto before = position_;
            const auto port = start();
            attributes();
            if (direction()) {
                bump();
            }
            while (type()) {
                bump();
            }
            if (name() && name(1)) {
                bump();
            }
            while (at("[")) {
                array_range();
            }
            expect_name();
            while (at("[")) {
                array_range();
            }
            if (eat("=")) {
                expression();
            }
            complete(port, SyntaxKind::Port);
            if (!eat(",")) {
                break;
            }
            progress(before);
        }
        expect(")");
        complete(list, SyntaxKind::ModulePortList);
    }
    void module_parameters() {
        const auto list = start();
        expect("#");
        expect("(");
        while (!eof() && !at(")") && !at(";")) {
            const auto before = position_;
            const auto parameter = start();
            attributes();
            eat("parameter");
            while (type()) {
                bump();
            }
            declarator(true);
            complete(parameter, SyntaxKind::ParameterDecl);
            if (!eat(",")) {
                break;
            }
            progress(before);
        }
        expect(")");
        complete(list, SyntaxKind::ModuleParameterList);
    }
    void module(Marker item) {
        bump();
        expect_name();
        if (at("#")) {
            module_parameters();
        }
        if (at("(")) {
            ports();
        }
        semicolon();
        while (!eof() && !at("endmodule") && !at("module") && !at("macromodule")) {
            const auto before = position_;
            const auto member = start();
            attributes();
            if (declaration_start()) {
                declaration(member);
            } else if (eat("analog")) {
                if (at("function")) {
                    function(member);
                } else {
                    eat("initial");
                    statement();
                    complete(member, SyntaxKind::AnalogBlock);
                }
            } else if (at("function")) {
                function(member);
            } else if (eat(";")) {
                complete(member, SyntaxKind::EmptyStmt);
            } else {
                error("PARSE_MODULE_ITEM", "unsupported or invalid module item");
                recover({";", "endmodule", "analog", "parameter", "real", "integer", "module"});
                eat(";");
                complete(member, SyntaxKind::ErrorNode);
            }
            progress(before);
        }
        expect("endmodule");
        if (eat(":")) {
            expect_name();
        }
        complete(item, SyntaxKind::ModuleDecl);
    }
    void physical_type(Marker item) {
        const bool nature = at("nature");
        const auto end = nature ? "endnature" : "enddiscipline";
        bump();
        expect_name();
        if (eat(":")) {
            expect_name();
        }
        semicolon();
        while (!eof() && !at(end) && !at("module")) {
            const auto before = position_;
            const auto property = start();
            expect_name();
            eat("=");
            expression();
            semicolon();
            complete(property, SyntaxKind::PropertyDecl);
            progress(before);
        }
        expect(end);
        eat(";");
        complete(item, nature ? SyntaxKind::NatureDecl : SyntaxKind::DisciplineDecl);
    }
    void function(Marker item) {
        expect("function");
        while (type()) {
            bump();
        }
        expect_name();
        if (at("(")) {
            ports();
        }
        semicolon();
        while (!eof() && !at("endfunction") && !at("endmodule")) {
            const auto before = position_;
            statement();
            progress(before);
        }
        expect("endfunction");
        complete(item, SyntaxKind::AnalogFunctionDecl);
    }

    void statement() {
        const auto item = start();
        const Depth guard(depth_);
        if (depth_ > 128) {
            error("PARSE_DEPTH", "syntax nesting exceeds 128");
            recover({";", "end", "else", "endmodule", "endcase", "endfunction"});
            eat(";");
            complete(item, SyntaxKind::ErrorNode);
            return;
        }
        attributes();
        if (declaration_start()) {
            declaration(item);
        } else if (eat(";")) {
            complete(item, SyntaxKind::EmptyStmt);
        } else if (eat("begin")) {
            if (eat(":")) {
                expect_name();
            }
            while (!eof() && !one_of({"end", "endmodule", "endfunction", "else", "endcase"})) {
                const auto before = position_;
                statement();
                progress(before);
            }
            expect("end");
            if (eat(":")) {
                expect_name();
            }
            complete(item, SyntaxKind::BlockStmt);
        } else if (eat("if")) {
            parenthesized_condition();
            statement();
            if (eat("else")) {
                statement();
            }
            complete(item, SyntaxKind::IfStmt);
        } else if (at("case") || at("casex") || at("casez")) {
            case_statement(item);
        } else if (eat("for")) {
            expect("(");
            if (!at(";")) {
                simple_statement(false);
            }
            expect(";");
            if (!at(";")) {
                expression();
            }
            expect(";");
            if (!at(")")) {
                simple_statement(false);
            }
            expect(")");
            statement();
            complete(item, SyntaxKind::ForStmt);
        } else if (at("while") || at("repeat")) {
            const bool repeat = at("repeat");
            bump();
            parenthesized_condition();
            statement();
            complete(item, repeat ? SyntaxKind::RepeatStmt : SyntaxKind::WhileStmt);
        } else if (eat("@")) {
            const auto events = start();
            expect("(");
            expression();
            while (eat("or") || eat(",")) {
                expression();
            }
            expect(")");
            complete(events, SyntaxKind::EventList);
            statement();
            complete(item, SyntaxKind::EventStmt);
        } else if (one_of({"break", "continue", "disable", "return"})) {
            bump();
            if (!at(";") && !boundary()) {
                expression();
            }
            semicolon();
            complete(item, SyntaxKind::JumpStmt);
        } else if (boundary()) {
            error("PARSE_STATEMENT", "expected a statement");
            complete(item, SyntaxKind::ErrorNode);
        } else {
            simple_statement(true, item);
        }
    }
    void parenthesized_condition() {
        expect("(");
        expression();
        expect(")");
    }
    void case_statement(Marker item) {
        bump();
        parenthesized_condition();
        while (!eof() && !one_of({"endcase", "end", "endmodule", "endfunction"})) {
            const auto before = position_;
            const auto branch = start();
            if (eat("default")) {
                eat(":");
            } else {
                expression();
                while (eat(",")) {
                    expression();
                }
                expect(":");
            }
            statement();
            complete(branch, SyntaxKind::CaseItem);
            progress(before);
        }
        expect("endcase");
        complete(item, SyntaxKind::CaseStmt);
    }
    bool assignment_operator() const {
        return one_of({"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="});
    }
    void simple_statement(bool with_semicolon) {
        simple_statement(with_semicolon, start());
    }
    void simple_statement(bool with_semicolon, Marker item) {
        expression();
        auto kind = SyntaxKind::ExprStmt;
        if (assignment_operator() || at("<+")) {
            kind = at("<+") ? SyntaxKind::ContributionStmt : SyntaxKind::AssignmentStmt;
            bump();
            expression();
        }
        if (with_semicolon) {
            semicolon();
        }
        complete(item, kind);
    }

    // LRM 2.4 §4.2.2：所有二元算术运算符（含 **）左结合；?: 右结合。
    int binary_power() const {
        if (at("**")) return 23;
        if (one_of({"*", "/", "%"})) return 21;
        if (one_of({"+", "-"})) return 19;
        if (one_of({"<<", ">>", "<<<", ">>>"})) return 17;
        if (one_of({"<", "<=", ">", ">="})) return 15;
        if (one_of({"==", "!=", "===", "!=="})) return 13;
        if (at("&")) return 11;
        if (one_of({"^", "^~", "~^"})) return 9;
        if (at("|")) return 7;
        if (at("&&")) return 5;
        if (at("||")) return 3;
        return 0;
    }
    bool prefix() const {
        return one_of({"+", "-", "!", "~", "&", "~&", "|", "~|", "^", "~^", "^~", "++", "--"});
    }
    CompletedMarker expression(int minimum = 0) {
        const Depth guard(depth_);
        if (depth_ > 128) {
            const auto bad = start();
            error("PARSE_DEPTH", "syntax nesting exceeds 128");
            recover({";", ")", "]", "}", ",", "end", "else", "endmodule", "endfunction"});
            return complete(bad, SyntaxKind::ErrorNode);
        }
        auto left = primary();
        while (!eof()) {
            if (minimum <= 27 && at("(")) {
                const auto call = precede(left);
                arguments();
                left = complete(call, SyntaxKind::CallExpr);
            } else if (minimum <= 27 && at("[")) {
                const auto subscript = precede(left);
                bump();
                expression();
                if (eat(":") || eat("+:") || eat("-:")) {
                    expression();
                }
                expect("]");
                left = complete(subscript, SyntaxKind::IndexExpr);
            } else if (minimum <= 27 && eat(".")) {
                // '.' 已消费，父边仍将原左表达式与成员名包在一起。
                const auto member = precede(left);
                expect_name();
                left = complete(member, SyntaxKind::MemberExpr);
            } else if (minimum <= 27 && (at("++") || at("--"))) {
                const auto postfix = precede(left);
                bump();
                left = complete(postfix, SyntaxKind::PostfixExpr);
            } else if (minimum <= 1 && eat("?")) {
                const auto condition = precede(left);
                expression();
                expect(":");
                expression(1);
                left = complete(condition, SyntaxKind::ConditionalExpr);
            } else {
                const int power = binary_power();
                if (power == 0 || power < minimum) {
                    break;
                }
                const auto binary = precede(left);
                bump();
                expression(power + 1);
                left = complete(binary, SyntaxKind::BinaryExpr);
            }
        }
        return left;
    }
    CompletedMarker primary() {
        const auto item = start();
        if (prefix()) {
            bump();
            expression(25); // 一元运算符优先于幂运算
            return complete(item, SyntaxKind::UnaryExpr);
        }
        if (name()) {
            bump();
            return complete(item, SyntaxKind::NameExpr);
        }
        if (token().kind == TokenKind::NumberLiteral || token().kind == TokenKind::StringLiteral ||
            at("inf")) {
            bump();
            return complete(item, SyntaxKind::LiteralExpr);
        }
        if (eat("(")) {
            expression();
            expect(")");
            return complete(item, SyntaxKind::ParenExpr);
        }
        if (eat("{")) {
            expression();
            auto kind = SyntaxKind::ConcatExpr;
            if (at("{")) {
                // 仍经 expression 进入内层，保证重复连接也计入递归深度。
                expression(27);
                kind = SyntaxKind::ReplicationExpr;
            } else {
                while (eat(",")) {
                    expression();
                }
            }
            expect("}");
            return complete(item, kind);
        }
        error("PARSE_EXPRESSION", "expected an expression");
        if (!boundary()) {
            bump();
        }
        return complete(item, SyntaxKind::ErrorNode);
    }
    void arguments() {
        const auto list = start();
        expect("(");
        while (!eof() && !at(")") && !one_of({";", "end", "endmodule", "endfunction"})) {
            const auto before = position_;
            expression();
            if (!eat(",")) {
                break;
            }
            if (at(")")) {
                error("PARSE_EXPRESSION", "expected an expression after ','");
            }
            progress(before);
        }
        expect(")");
        complete(list, SyntaxKind::ArgumentList);
    }

    ParseResult result_;
    std::vector<std::size_t> significant_;
    std::size_t position_ = 0;
    std::size_t depth_ = 0;
};

} // namespace

ParseResult parse(std::shared_ptr<const PreprocessResult> input) {
    return Parser(std::move(input)).run();
}

} // namespace vagrad
