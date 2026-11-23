#include "vagrad/frontend/hir/typed.hpp"
#include "vagrad/frontend/ast/ast.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace vagrad::hir {
namespace {
bool errors(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const auto& d) { return d.severity == Severity::Error; });
}
}

bool HirModule::has_errors() const { return bindings->has_errors() || errors(diagnostics); }
bool numeric(Type type) { return type == Type::Real || type == Type::Integer; }
Type scalar_type(const Scalar& value) {
    if (std::holds_alternative<double>(value)) return Type::Real;
    if (std::holds_alternative<std::int32_t>(value)) return Type::Integer;
    return Type::String;
}
double number(const Scalar& value) {
    if (const auto p = std::get_if<double>(&value)) return *p;
    return static_cast<double>(std::get<std::int32_t>(value));
}
std::string scalar_text(const Scalar& value) {
    std::ostringstream out;
    out << std::setprecision(17);
    if (const auto p = std::get_if<std::string>(&value)) out << std::quoted(*p);
    else if (const auto real = std::get_if<double>(&value)) out << *real;
    else out << std::get<std::int32_t>(value);
    return out.str();
}
std::string_view type_name(Type type) {
    switch (type) {
    case Type::Real: return "Real"; case Type::Integer: return "Integer";
    case Type::String: return "String"; case Type::Net: return "Net";
    case Type::Branch: return "Branch"; case Type::Void: return "Void";
    default: return "Error";
    }
}
std::string_view expr_kind_name(ExprKind kind) {
    switch (kind) {
    case ExprKind::Literal: return "Literal"; case ExprKind::Ref: return "Ref";
    case ExprKind::Convert: return "Convert"; case ExprKind::Unary: return "Unary";
    case ExprKind::Binary: return "Binary"; case ExprKind::Select: return "Select";
    case ExprKind::ReadPotential: return "ReadPotential"; case ExprKind::Temperature: return "Temperature";
    case ExprKind::MathCall: return "PureMathCall"; case ExprKind::DiagnosticCall: return "DiagnosticCall";
    default: return "Error";
    }
}
std::string_view stmt_kind_name(StmtKind kind) {
    switch (kind) {
    case StmtKind::Empty: return "Empty"; case StmtKind::Block: return "Block";
    case StmtKind::If: return "If"; case StmtKind::Assign: return "Assign";
    case StmtKind::Contribute: return "ContributeFlow"; default: return "Expression";
    }
}
Diagnostic semantic_diagnostic(const BindingResult& bindings, Origin origin,
                               std::string code, std::string message) {
    const ast::Token token(bindings.syntax, origin.token_index);
    Diagnostic d{Severity::Error, std::move(code), std::move(message), token.source_span(), {}};
    auto context = std::optional<std::size_t>{token.context_index()};
    while (context) {
        const auto& item = bindings.syntax.input().contexts.at(*context);
        if (item.call_site) d.related.push_back({*item.call_site,
            item.kind == ExpansionKind::Include ? "included from here" : "expanded from here"});
        if (item.definition) d.related.push_back({*item.definition, "macro definition/formal"});
        context = item.parent;
    }
    return d;
}

const std::vector<BuiltinSignature>& builtin_registry() {
    static const std::vector<BuiltinSignature> registry = [] {
        std::vector<BuiltinSignature> r;
        for (const auto name : {"abs", "sqrt", "exp", "ln", "log", "sin", "cos", "tan",
             "asin", "acos", "atan", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "ceil", "floor"})
            r.push_back({name, Effect::PureMath, 1, 1, {ArgumentType::Numeric}, {}, Type::Real});
        for (const auto name : {"min", "max", "atan2"})
            r.push_back({name, Effect::PureMath, 2, 2, {ArgumentType::Numeric, ArgumentType::Numeric}, {}, Type::Real});
        r.push_back({"$vt", Effect::Environment, 0, 1, {ArgumentType::Numeric}, {}, Type::Real});
        for (const auto name : {"$strobe", "$display", "$write"})
            r.push_back({name, Effect::Diagnostic, 1, 64, {ArgumentType::String}, ArgumentType::Scalar, Type::Void});
        for (const auto name : {"limexp", "ddt", "idt", "transition", "slew", "white_noise", "flicker_noise"})
            r.push_back({name, Effect::Stateful, 0, 64, {}, ArgumentType::Scalar, Type::Error});
        return r;
    }();
    return registry;
}

std::optional<Scalar> decode_number(std::string_view text, LiteralFormat& format, std::string& error) {
    format = {};
    format.spelling = std::string(text);
    error.clear();
    std::string s;
    for (const auto c : text) if (c != '_') s += c;
    auto bad = [&](std::string message) -> std::optional<Scalar> {
        error = std::move(message); return std::nullopt;
    };
    if (s.empty()) return bad("empty numeric literal");
    const auto quote = s.find('\'');
    if (quote != s.npos) {
        unsigned width = 32;
        if (quote) {
            const auto p = std::from_chars(s.data(), s.data() + quote, width);
            if (p.ec != std::errc{} || p.ptr != s.data() + quote || width == 0 || width > 32)
                return bad("only based literal widths 1..32 are supported");
        }
        format.width = width;
        auto pos = quote + 1;
        if (pos < s.size() && (s[pos] == 's' || s[pos] == 'S')) {
            format.explicitly_signed = true; ++pos;
        }
        if (pos == s.size()) return bad("missing based literal radix");
        const auto radix = s[pos++];
        switch (radix) {
        case 'b': case 'B': format.radix = 2; break;
        case 'o': case 'O': format.radix = 8; break;
        case 'd': case 'D': format.radix = 10; break;
        case 'h': case 'H': format.radix = 16; break;
        default: return bad("invalid based literal radix");
        }
        std::uint64_t value = 0;
        const auto p = std::from_chars(s.data() + pos, s.data() + s.size(), value, static_cast<int>(format.radix));
        if (p.ec != std::errc{} || p.ptr != s.data() + s.size())
            return bad("invalid digits or unsupported x/z/? in numeric literal");
        const auto mask = (std::uint64_t{1} << width) - 1;
        if (value > mask) return bad("digits exceed declared literal width (truncation is outside this profile)");
        std::int64_t signed_value = static_cast<std::int64_t>(value);
        if (format.explicitly_signed && (value & (std::uint64_t{1} << (width - 1))))
            signed_value -= static_cast<std::int64_t>(std::uint64_t{1} << width);
        if (signed_value > INT32_MAX) return bad("unsigned value exceeds supported signed Integer range");
        return Scalar{static_cast<std::int32_t>(signed_value)};
    }
    double scale = 1.0;
    bool suffix = true;
    switch (s.back()) {
    case 'T': scale = 1e12; break; case 'G': scale = 1e9; break;
    case 'M': scale = 1e6; break; case 'K': case 'k': scale = 1e3; break;
    case 'm': scale = 1e-3; break; case 'u': scale = 1e-6; break;
    case 'n': scale = 1e-9; break; case 'p': scale = 1e-12; break;
    case 'f': scale = 1e-15; break; case 'a': scale = 1e-18; break;
    default: suffix = false;
    }
    if (suffix) s.pop_back();
    if (!suffix && s.find_first_of(".eE") == s.npos) {
        std::int32_t value = 0;
        const auto p = std::from_chars(s.data(), s.data() + s.size(), value);
        if (p.ec != std::errc{} || p.ptr != s.data() + s.size())
            return bad("invalid decimal or value outside signed 32-bit Integer range");
        return Scalar{value};
    }
    // from_chars 不受本机小数点区域设置影响，并要求消费完整字符串。
    double value = 0;
    const auto p = std::from_chars(s.data(), s.data() + s.size(), value, std::chars_format::general);
    value *= scale;
    if (s.empty() || p.ec != std::errc{} || p.ptr != s.data() + s.size() || !std::isfinite(value))
        return bad("invalid or non-finite Real literal");
    return Scalar{value};
}

void print_typed_hir(std::ostream& out, const HirModule& m) {
    out << "HIR H2 module=" << m.name << " parameters=" << m.parameters.size()
        << " variables=" << m.variables.size() << " expressions=" << m.expressions.size()
        << " statements=" << m.statements.size() << " errors="
        << std::count_if(m.diagnostics.begin(), m.diagnostics.end(),
            [](const auto& d) { return d.severity == Severity::Error; }) << '\n';
    print_source_map(out, m.bindings->syntax.input());
    const auto symbol = [&](SymbolId id) -> const std::string& { return m.bindings->symbol(id).name; };
    auto probe = [&](const Probe& p) {
        out << '(' << (p.positive ? symbol(*p.positive) : "0") << ", "
            << (p.negative ? symbol(*p.negative) : "0") << ')';
        if (p.alias) out << " alias=" << symbol(*p.alias);
    };
    for (const auto& p : m.parameters) {
        out << "Param [" << p.symbol.value << "] " << symbol(p.symbol) << ": " << type_name(p.type);
        if (p.default_value) out << " default=e" << p.default_value->value;
        out << " overridable=" << (p.overridable ? "true" : "false") << '\n';
        for (const auto& [key, value] : p.attributes) out << "  attribute " << key << '=' << value.text
            << " @node=" << value.origin.node.value << " token=" << value.origin.token_index << '\n';
        for (const auto& range : p.constraints) {
            out << "  " << (range.exclude ? "exclude " : "from ");
            auto bound = [&](const Bound& b) {
                if (b.kind == Bound::Kind::PositiveInfinity) out << "+inf";
                else if (b.kind == Bound::Kind::NegativeInfinity) out << "-inf";
                else if (b.expression) out << 'e' << b.expression->value;
                else out << "<missing>";
            };
            if (range.interval) out << (range.lower_inclusive ? '[' : '(');
            bound(range.lower);
            if (range.interval) { out << ':'; bound(range.upper); out << (range.upper_inclusive ? ']' : ')'); }
            out << '\n';
        }
    }
    for (const auto& v : m.variables)
        out << "Variable [" << v.symbol.value << "] " << symbol(v.symbol) << ": " << type_name(v.type) << '\n';
    for (const auto& b : m.branches) { out << "Branch " << symbol(b.symbol); probe(b.probe); out << '\n'; }
    for (std::size_t i = 0; i < m.expressions.size(); ++i) {
        const auto& e = m.expressions[i];
        out << 'e' << i << ": " << type_name(e.type) << " = " << expr_kind_name(e.kind);
        if (!e.op.empty()) out << ' ' << e.op;
        if (e.symbol) out << " [" << e.symbol->value << "] " << symbol(*e.symbol);
        if (e.kind == ExprKind::Literal) out << ' ' << scalar_text(e.literal);
        if (e.format) {
            out << " spelling=" << e.format->spelling;
            if (e.type == Type::Integer) out << " width=" << e.format->width
                << " radix=" << e.format->radix << " signed=" << e.format->explicitly_signed;
        }
        if (e.probe) probe(*e.probe);
        for (const auto operand : e.operands) out << " e" << operand.value;
        out << " @node=" << e.origin.node.value << " token=" << e.origin.token_index << '\n';
    }
    for (std::size_t i = 0; i < m.statements.size(); ++i) {
        const auto& s = m.statements[i];
        out << 's' << i << " = " << stmt_kind_name(s.kind);
        if (s.target) out << " [" << s.target->value << "] " << symbol(*s.target);
        if (s.probe) probe(*s.probe);
        if (s.expression) out << " e" << s.expression->value;
        for (const auto child : s.children) out << " s" << child.value;
        out << " @node=" << s.origin.node.value << " token=" << s.origin.token_index << '\n';
    }
    if (m.body) out << "Entry s" << m.body->value << '\n';
    for (const auto& d : m.diagnostics) out << "Diagnostic " << d.code << ": " << d.message << '\n';
}
} // namespace vagrad::hir
