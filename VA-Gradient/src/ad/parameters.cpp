#include "vagrad/ad/plan.hpp"
#include "vagrad/runtime/scalar.hpp"
#include <algorithm>
#include <map>

namespace vagrad::ad {
namespace {
namespace H=hir;
struct Dual {H::Scalar value;std::vector<double> tangent;bool active=false;};
class Defaults {
public:
    Defaults(const Plan& p,const H::ParamEnv& e):plan(p),env(e),ops(*p.hir,diagnostics) {
        for(const auto& parameter:plan.hir->parameters) parameters[parameter.symbol.value]=&parameter;
    }
    const Plan& plan;const H::ParamEnv& env;
    std::vector<Diagnostic> diagnostics;runtime::ScalarOps ops;
    std::map<std::size_t,const H::ParamInfo*> parameters;
    std::map<std::size_t,Dual> cache;
    std::vector<std::size_t> visiting;
    std::vector<double> zero() const {return std::vector<double>(plan.parameters.size());}
    Dual resolve(H::SymbolId id) {
        if(cache.contains(id.value)) return cache.at(id.value);
        const auto found=parameters.find(id.value);
        if(found==parameters.end()) throw Error("default dependency is not a parameter");
        const auto& p=*found->second;
        if(id.value>=env.values.size()||!env.values[id.value])
            throw ParameterError(p.origin,"missing effective parameter value");
        if(visiting.size()>256||std::find(visiting.begin(),visiting.end(),id.value)!=visiting.end())
            throw ParameterError(p.origin,"cyclic or over-deep AD parameter defaults");
        const auto seed_lane=std::find(plan.parameters.begin(),plan.parameters.end(),id);
        Dual r{*env.values[id.value],zero(),false};
        if(seed_lane!=plan.parameters.end()) {
            r.tangent[static_cast<std::size_t>(seed_lane-plan.parameters.begin())]=1;r.active=true;
        } else if(std::find(env.overridden.begin(),env.overridden.end(),id)==env.overridden.end()) {
            if(!p.default_value) throw ParameterError(p.origin,"parameter has no default expression");
            visiting.push_back(id.value);r=expr(*p.default_value,0);visiting.pop_back();
        }
        // 原值采用已通过范围检查的有效环境，避免重算默认式的浮点误差改变原执行路径。
        r.value=*env.values[id.value];
        cache[id.value]=r;return r;
    }
    Jet jet(const Dual& d) const {return {H::number(d.value),d.tangent,d.active};}
    Dual dual(H::Scalar value,const Jet& j) const {return {std::move(value),j.tangent,j.active};}
    Dual expr(H::ExprId id,std::size_t depth) {
        const auto& e=plan.hir->expr(id);
        try {return body(e,depth);}
        catch(const ParameterError&){throw;}
        catch(const Error& x){throw ParameterError(e.origin,x.what());}
        catch(const runtime::Stop&) {
            throw ParameterError(e.origin,diagnostics.empty()?"invalid parameter default":diagnostics.back().message);
        }
    }
    Dual body(const H::Expr& e,std::size_t depth) {
        if(depth>256) throw Error("AD parameter expression nesting limit");
        auto argument=[&](std::size_t k){return expr(e.operands.at(k),depth+1);};
        switch(e.kind) {
        case H::ExprKind::Literal:return {e.literal,zero(),false};
        case H::ExprKind::Ref:return resolve(*e.symbol);
        case H::ExprKind::Convert: {
            auto a=argument(0);
            if(e.type==H::Type::Integer&&a.active) throw Error("active default Real to Integer conversion");
            a.value=ops.convert(a.value,e.type,e.origin);return a;
        }
        case H::ExprKind::Unary: {
            auto a=argument(0);const auto value=ops.unary(e.op,e.type,a.value,e.origin);
            if(e.type==H::Type::Integer) {
                if(a.active&&H::number(a.value)==0) throw Error("active default Boolean boundary");
                return {value,zero(),false};
            }
            if(e.op=="-") for(auto& d:a.tangent) d=-d;
            return {value,a.tangent,a.active};
        }
        case H::ExprKind::Binary: {
            const auto a=argument(0);
            if(e.op=="&&"||e.op=="||") {
                if(a.active&&H::number(a.value)==0) throw Error("active default logical boundary");
                if((e.op=="&&"&&H::number(a.value)==0)||(e.op=="||"&&H::number(a.value)!=0))
                    return {std::int32_t{e.op=="||"?1:0},zero(),false};
            }
            const auto b=argument(1);
            const auto value=ops.binary(e.op,e.type,a.value,b.value,e.origin);
            if(e.op=="&&"||e.op=="||") {
                if(b.active&&H::number(b.value)==0) throw Error("active default logical boundary");
                return {value,zero(),false};
            }
            if(e.type==H::Type::Integer) {
                if((a.active||b.active)&&H::number(a.value)==H::number(b.value)&&e.operands[0]!=e.operands[1])
                    throw Error("active default comparison boundary");
                return {value,zero(),false};
            }
            return dual(value,binary(e.op,jet(a),jet(b)));
        }
        case H::ExprKind::Select: {
            const auto c=argument(0);
            if(c.active&&H::number(c.value)==0) throw Error("active default conditional boundary");
            return argument(H::number(c.value)!=0?1:2);
        }
        case H::ExprKind::MathCall: {
            std::vector<Dual> a;std::vector<H::Scalar> values;
            for(std::size_t k=0;k<e.operands.size();++k) {a.push_back(argument(k));values.push_back(a.back().value);}
            const auto value=ops.math(e.op,values,e.origin);
            const bool active=std::any_of(a.begin(),a.end(),[](const auto& x){return x.active;});
            if(!active) return {value,zero(),false};
            if(e.op=="min"||e.op=="max") {
                if(H::number(a[0].value)==H::number(a[1].value)&&e.operands[0]!=e.operands[1])
                    throw Error("active default min/max boundary");
                return dual(value,select_extremum(e.op=="max",jet(a[0]),jet(a[1])));
            }
            if(a.size()==1) return dual(value,math(e.op,jet(a[0])));
            if(a.size()==2) return dual(value,math_binary(e.op,jet(a[0]),jet(a[1])));
            throw Error("unsupported AD parameter math rule");
        }
        default:throw Error("unsupported AD parameter default expression");
        }
    }
};
}
Jet default_parameter_jet(const Plan& p,const hir::ParamEnv& env,hir::SymbolId id) {
    Defaults evaluator(p,env);const auto d=evaluator.resolve(id);
    return {hir::number(d.value),d.tangent,d.active};
}
} // namespace vagrad::ad
