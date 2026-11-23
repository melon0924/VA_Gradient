#pragma once
#include <stdexcept>
#include <string_view>
#include <vector>

namespace vagrad::ad {
// 每个分量对应一个独立实数参数种子；这里提供局部数值传播规则。
// SSA/phi、稀疏持久存储、Origin 诊断由 state/eval 适配。
struct Jet {double value;std::vector<double> tangent;bool active=false;};
struct Error : std::runtime_error {using std::runtime_error::runtime_error;};
Jet constant(double value,std::size_t lanes);
Jet seed(double value,std::size_t lanes,std::size_t lane);
Jet binary(std::string_view op,const Jet& a,const Jet& b);
Jet math(std::string_view function,const Jet& a);
Jet math_binary(std::string_view function,const Jet& a,const Jet& b);
Jet select_extremum(bool maximum,const Jet& a,const Jet& b);
} // namespace vagrad::ad
