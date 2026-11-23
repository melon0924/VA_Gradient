#include "vagrad/runtime/scalar.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace vagrad::runtime {
using namespace hir;
ScalarOps::ScalarOps(const HirModule& module,std::vector<Diagnostic>& ds):m(module),diagnostics(ds) {}
[[noreturn]] void ScalarOps::fail(Origin origin, std::string code, std::string message) {
    diagnostics.push_back(semantic_diagnostic(*m.bindings, origin, std::move(code), std::move(message)));
    throw Stop{};
}
void ScalarOps::tick(Origin origin) {
    if (++steps > limit) fail(origin, "EVAL_LIMIT", "evaluation step limit exceeded");
}
Scalar ScalarOps::finite(Scalar value, Origin origin) {
    if (const auto p = std::get_if<double>(&value); p && !std::isfinite(*p))
        fail(origin, "EVAL_DOMAIN", "operation produced a non-finite Real (domain error or overflow)");
    return value;
}
Scalar ScalarOps::integer(double value, Origin origin, bool round) {
    value = round ? std::round(value) : std::trunc(value);
    if (!std::isfinite(value) || value < INT32_MIN || value > INT32_MAX)
        fail(origin, "EVAL_INTEGER_RANGE", "Integer result exceeds supported signed 32-bit range");
    return static_cast<std::int32_t>(value);
}
Scalar ScalarOps::convert(Scalar value, Type target, Origin origin) {
    const auto from = scalar_type(value);
    finite(value, origin);
    if (from == target) return value;
    if (numeric(from) && target == Type::Integer) return integer(number(value), origin);
    if (numeric(from) && target == Type::Real) return number(value);
    fail(origin, "EVAL_TYPE", "incompatible scalar value for " + std::string(type_name(target)));
}
double ScalarOps::temperature(Origin origin) {
    if (!inputs || !inputs->temperature_kelvin)
        fail(origin, "EVAL_INPUT", "missing explicit temperature_kelvin input");
    const auto t = *inputs->temperature_kelvin;
    if (!std::isfinite(t) || t <= 0) fail(origin, "EVAL_INPUT", "temperature must be finite and positive in kelvin");
    return t;
}
double ScalarOps::voltage(std::optional<SymbolId> id, Origin origin) {
    if (!id) return 0.0;
    if (!inputs) fail(origin, "EVAL_PARAMETER_CONSTANT", "potential read in parameter expression");
    const auto& name = m.bindings->symbol(*id).name;
    const auto p = inputs->node_voltages.find(name);
    if (p == inputs->node_voltages.end()) fail(origin, "EVAL_INPUT", "missing node voltage: " + name);
    if (!std::isfinite(p->second)) fail(origin, "EVAL_INPUT", "node voltage must be finite: " + name);
    return p->second;
}
Scalar ScalarOps::unary(std::string_view op,Type type,const Scalar& value,Origin origin) {
    const auto x=number(value);
    if(op=="+") return value;
    if(op=="!") return std::int32_t{x==0?1:0};
    if(op=="~") return static_cast<std::int32_t>(~std::get<std::int32_t>(value));
    if(op!="-") fail(origin,"EVAL_HIR","unknown unary operation");
    return type==Type::Integer?integer(-x,origin):finite(-x,origin);
}
Scalar ScalarOps::binary(std::string_view op,Type type,const Scalar& left,const Scalar& right,Origin origin) {
    const auto a=number(left);
    const auto b = number(right);
    const auto truth = [](bool value) -> Scalar { return std::int32_t{value ? 1 : 0}; };
    if (op == "&&") return truth(a != 0 && b != 0);
    if (op == "||") return truth(a != 0 || b != 0);
    if (op == "<") return truth(a < b); if (op == "<=") return truth(a <= b);
    if (op == ">") return truth(a > b); if (op == ">=") return truth(a >= b);
    if (op == "==") return truth(a == b); if (op == "!=") return truth(a != b);
    const auto bits = [&](const Scalar& v) { return std::bit_cast<std::uint32_t>(std::get<std::int32_t>(v)); };
    const auto signed_bits = [](std::uint32_t v) -> Scalar { return std::bit_cast<std::int32_t>(v); };
    if (op == "&") return signed_bits(bits(left) & bits(right));
    if (op == "|") return signed_bits(bits(left) | bits(right));
    if (op == "^") return signed_bits(bits(left) ^ bits(right));
    if (op == "^~" || op == "~^") return signed_bits(~(bits(left) ^ bits(right)));
    if (op == "<<" || op == "<<<" || op == ">>" || op == ">>>") {
        const auto count = std::get<std::int32_t>(right);
        if (count < 0 || count > 31) fail(origin, "EVAL_INTEGER_RANGE", "shift count must be in 0..31");
        auto value = bits(left);
        if (op == "<<" || op == "<<<") value <<= count;
        else {
            const bool negative = (value & 0x80000000u) != 0;
            value >>= count;
            if (op == ">>>" && negative && count > 0) value |= (~std::uint32_t{0} << (32 - count));
        }
        return signed_bits(value);
    }
    if ((op == "/" || op == "%") && b == 0) fail(origin, "EVAL_DIV_ZERO", "division/remainder by zero");
    double result = 0;
    if (op == "+") result = a + b; else if (op == "-") result = a - b;
    else if (op == "*") {
        // int32 乘法用 int64，避免超过 2^53 后的双精度舍入掩盖溢出。
        if (type == Type::Integer) {
            const auto x = static_cast<std::int64_t>(std::get<std::int32_t>(left));
            const auto y = static_cast<std::int64_t>(std::get<std::int32_t>(right));
            const auto product = x * y;
            if (product < INT32_MIN || product > INT32_MAX)
                fail(origin, "EVAL_INTEGER_RANGE", "Integer multiplication overflow");
            return static_cast<std::int32_t>(product);
        }
        result = a * b;
    } else if (op == "/") result = a / b;
    else if (op == "%") result = static_cast<double>(
        static_cast<std::int64_t>(std::get<std::int32_t>(left)) %
        static_cast<std::int64_t>(std::get<std::int32_t>(right)));
    else if (op == "**") result = std::pow(a, b);
    else fail(origin, "EVAL_HIR", "unknown binary operation");
    return type == Type::Integer ? integer(result, origin, false) : finite(result, origin);
}
Scalar ScalarOps::math(std::string_view op,const std::vector<Scalar>& values,Origin origin) {
    std::vector<double> args;
    for (const auto& value : values) args.push_back(number(value));
    if (op == "$vt") {
        const auto t = args.empty() ? temperature(origin) : args[0];
        if (!std::isfinite(t) || t <= 0) fail(origin, "EVAL_DOMAIN", "$vt requires positive kelvin temperature");
        return finite(thermal_voltage_per_kelvin * t, origin);
    }
    const auto x = args.at(0);
    double result = 0;
    if (op == "min") result = std::min(x, args[1]); else if (op == "max") result = std::max(x, args[1]);
    else if (op == "atan2") result = std::atan2(x, args[1]);
    else if (op == "abs") result = std::abs(x); else if (op == "sqrt") result = std::sqrt(x);
    else if (op == "exp") result = std::exp(x); else if (op == "ln") result = std::log(x);
    else if (op == "log") result = std::log10(x); else if (op == "sin") result = std::sin(x);
    else if (op == "cos") result = std::cos(x); else if (op == "tan") result = std::tan(x);
    else if (op == "asin") result = std::asin(x); else if (op == "acos") result = std::acos(x);
    else if (op == "atan") result = std::atan(x); else if (op == "sinh") result = std::sinh(x);
    else if (op == "cosh") result = std::cosh(x); else if (op == "tanh") result = std::tanh(x);
    else if (op == "asinh") result = std::asinh(x); else if (op == "acosh") result = std::acosh(x);
    else if (op == "atanh") result = std::atanh(x); else if (op == "ceil") result = std::ceil(x);
    else if (op == "floor") result = std::floor(x);
    else fail(origin, "EVAL_HIR", "unknown math operation");
    return finite(result, origin);
}
} // namespace vagrad::runtime
