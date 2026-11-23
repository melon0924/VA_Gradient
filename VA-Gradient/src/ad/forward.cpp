#include "vagrad/ad/forward.hpp"
#include "vagrad/runtime/scalar.hpp"
#include <algorithm>
#include <cmath>

namespace vagrad::ad {
namespace {
Jet checked(Jet x) {
    if(!std::isfinite(x.value)) throw Error("AD primal domain error");
    for(const auto d:x.tangent) {
        if(!std::isfinite(d)) throw Error("AD tangent is non-finite");
        if(d!=0) x.active=true;
    }
    return x;
}
void compatible(const Jet& a,const Jet& b) {
    (void)checked(a);(void)checked(b);
    if(a.tangent.size()!=b.tangent.size()) throw Error("AD seed dimensions differ");
}
}
Jet constant(double value,std::size_t lanes) {return checked({value,std::vector<double>(lanes)});}
Jet seed(double value,std::size_t lanes,std::size_t lane) {
    if(lane>=lanes) throw Error("invalid AD seed lane");
    auto x=constant(value,lanes);x.tangent[lane]=1;x.active=true;return x;
}
Jet binary(std::string_view op,const Jet& left,const Jet& right) {
    const auto a=checked(left),b=checked(right);
    compatible(a,b);
    auto r=constant(0,a.tangent.size());
    r.active=a.active||b.active;
    if(op=="+") r.value=a.value+b.value;
    else if(op=="-") r.value=a.value-b.value;
    else if(op=="*") r.value=a.value*b.value;
    else if(op=="/") {if(b.value==0) throw Error("AD division by zero");r.value=a.value/b.value;}
    else if(op=="**") {
        r.value=std::pow(a.value,b.value);
        if(a.value<=0&&b.active) throw Error("variable exponent requires a positive base");
        if(a.value<0&&std::floor(b.value)!=b.value) throw Error("non-integer power of a negative base");
        if(a.value==0&&a.active&&std::floor(b.value)!=b.value) throw Error("active power at a domain boundary");
    }
    else throw Error("unsupported AD binary rule");
    for(std::size_t k=0;k<r.tangent.size();++k) {
        if(op=="+") r.tangent[k]=a.tangent[k]+b.tangent[k];
        else if(op=="-") r.tangent[k]=a.tangent[k]-b.tangent[k];
        else if(op=="*") r.tangent[k]=a.tangent[k]*b.value+a.value*b.tangent[k];
        else if(op=="/") r.tangent[k]=(a.tangent[k]-r.value*b.tangent[k])/b.value;
        else if(a.value>0) {
            r.tangent[k]=a.tangent[k]==0||b.value==0?0:b.value*std::pow(a.value,b.value-1)*a.tangent[k];
            if(b.tangent[k]!=0) r.tangent[k]+=r.value*std::log(a.value)*b.tangent[k];
        }
        else if(b.value==0||a.tangent[k]==0) r.tangent[k]=0;
        else if(a.value<0&&r.value!=0) r.tangent[k]=b.value*(r.value/a.value)*a.tangent[k];
        else r.tangent[k]=b.value*std::pow(a.value,b.value-1)*a.tangent[k];
    }
    return checked(std::move(r));
}
Jet math(std::string_view name,const Jet& input) {
    const auto a=checked(input);
    auto r=constant(0,a.tangent.size());r.active=a.active;double slope=0;
    if(name=="sqrt") {
        if(a.value<0) throw Error("sqrt domain error");
        r.value=std::sqrt(a.value);
        if(r.value==0) {
            if(a.active) throw Error("sqrt derivative singular at zero");
        } else slope=.5/r.value;
    } else if(name=="exp") {r.value=std::exp(a.value);slope=r.value;}
    else if(name=="ln") {
        if(a.value<=0) throw Error("ln domain error");
        r.value=std::log(a.value);slope=1/a.value;
    } else if(name=="log") {
        if(a.value<=0) throw Error("log domain error");
        r.value=std::log10(a.value);slope=(1/a.value)/std::log(10.0);
    } else if(name=="sin") {r.value=std::sin(a.value);slope=std::cos(a.value);}
    else if(name=="cos") {r.value=std::cos(a.value);slope=-std::sin(a.value);}
    else if(name=="tan") {r.value=std::tan(a.value);const auto c=std::cos(a.value);slope=1/(c*c);}
    else if(name=="asin") {r.value=std::asin(a.value);slope=1/std::sqrt(1-a.value*a.value);}
    else if(name=="acos") {r.value=std::acos(a.value);slope=-1/std::sqrt(1-a.value*a.value);}
    else if(name=="atan") {r.value=std::atan(a.value);const auto inv=1/std::hypot(a.value,1.0);slope=inv*inv;}
    else if(name=="sinh") {r.value=std::sinh(a.value);slope=std::cosh(a.value);}
    else if(name=="cosh") {r.value=std::cosh(a.value);slope=std::sinh(a.value);}
    else if(name=="tanh") {
        r.value=std::tanh(a.value);
        const auto e=std::exp(-2*std::abs(a.value));slope=4*e/((1+e)*(1+e));
    }
    else if(name=="asinh") {r.value=std::asinh(a.value);slope=1/std::hypot(a.value,1.0);}
    else if(name=="acosh") {r.value=std::acosh(a.value);slope=1/(std::sqrt(a.value-1)*std::sqrt(a.value+1));}
    else if(name=="atanh") {r.value=std::atanh(a.value);slope=1/(1-a.value*a.value);}
    else if(name=="abs") {
        r.value=std::abs(a.value);
        if(a.active&&a.value==0) throw Error("active abs boundary");
        slope=a.value>0?1:a.value<0?-1:0;
    } else if(name=="ceil"||name=="floor") {
        r.value=name=="ceil"?std::ceil(a.value):std::floor(a.value);
        if(a.active&&std::floor(a.value)==a.value) throw Error("active rounding boundary");
        r.active=false; // 远离跳变点时，舍入结果在局部为常量。
    } else if(name=="$vt") {
        if(a.value<=0) throw Error("$vt requires positive kelvin temperature");
        r.value=runtime::thermal_voltage_per_kelvin*a.value;slope=runtime::thermal_voltage_per_kelvin;
    } else throw Error("unsupported AD math rule");
    if(a.active&&!std::isfinite(slope)) throw Error("AD math derivative singular");
    if(!a.active) return checked(std::move(r));
    for(std::size_t k=0;k<r.tangent.size();++k) r.tangent[k]=slope*a.tangent[k];
    return checked(std::move(r));
}
Jet math_binary(std::string_view name,const Jet& left,const Jet& right) {
    const auto a=checked(left),b=checked(right);
    compatible(a,b);
    if(name!="atan2") throw Error("unsupported binary AD math rule");
    auto r=constant(std::atan2(a.value,b.value),a.tangent.size());r.active=a.active||b.active;
    const auto scale=std::max(std::abs(a.value),std::abs(b.value));
    if(scale==0) {
        if(r.active) throw Error("atan2 derivative undefined at the origin");
        return r;
    }
    const auto x=a.value/scale,y=b.value/scale,denominator=x*x+y*y;
    for(std::size_t k=0;k<r.tangent.size();++k)
        r.tangent[k]=(y/denominator)*(a.tangent[k]/scale)-(x/denominator)*(b.tangent[k]/scale);
    return checked(std::move(r));
}
Jet select_extremum(bool maximum,const Jet& a,const Jet& b) {
    compatible(a,b);
    if(a.value==b.value) {
        if(a.tangent!=b.tangent) throw Error("min/max derivative is ambiguous at equal inputs");
        auto r=a;r.active=a.active||b.active;return checked(std::move(r));
    }
    return checked((maximum?a.value>b.value:a.value<b.value)?a:b);
}
} // namespace vagrad::ad
