#include "vagrad/ad/ir.hpp"
#include <algorithm>
#include <cmath>
#include <ostream>

namespace vagrad::ad {
namespace {
namespace M=mir;namespace H=hir;
DerivativeKind classify(const M::Instruction& i,H::Type type) {
    if(i.opcode==M::Opcode::Undef) return DerivativeKind::Undefined;
    if(i.opcode==M::Opcode::Phi) return DerivativeKind::Phi;
    if(i.opcode==M::Opcode::LoadParam&&H::numeric(type)) return DerivativeKind::Parameter;
    if(type==H::Type::Integer&&(i.opcode==M::Opcode::Convert||i.opcode==M::Opcode::Unary||i.opcode==M::Opcode::Binary))
        return DerivativeKind::Discrete;
    if(type!=H::Type::Real) return DerivativeKind::Zero;
    if(i.opcode==M::Opcode::Convert||i.opcode==M::Opcode::Unary) return DerivativeKind::Expression;
    if(i.opcode==M::Opcode::Binary) return i.op=="**"?DerivativeKind::Power:DerivativeKind::Expression;
    if(i.opcode==M::Opcode::MathCall) {
        if(i.operands.empty()) return DerivativeKind::Zero; // 无参 $vt 只读环境
        if(i.op=="min"||i.op=="max") return DerivativeKind::Extremum;
        return i.operands.size()==1?DerivativeKind::UnaryMath:DerivativeKind::BinaryMath;
    }
    return DerivativeKind::Zero;
}
DerivativeRule rule(const M::Instruction& i,H::Type type) {
    DerivativeRule r;r.result=*i.result;r.source=i.opcode;r.type=type;r.arguments=i.operands;r.operation=i.op;
    r.kind=classify(i,type);
    if(r.kind!=DerivativeKind::Expression) return r;
    auto leaf=[&](DerivativeOp op,std::size_t arg=0) {
        const auto id=r.nodes.size();r.nodes.push_back({op,{},arg});return id;
    };
    auto node=[&](DerivativeOp op,std::vector<std::size_t> args) {
        const auto id=r.nodes.size();r.nodes.push_back({op,std::move(args),0});return id;
    };
    const auto da=leaf(DerivativeOp::TangentArgument);
    if(i.opcode==M::Opcode::Convert||i.opcode==M::Opcode::Unary) {
        r.root=i.opcode==M::Opcode::Unary&&i.op=="-"?node(DerivativeOp::Negate,{da}):da;
    } else {
        const auto db=leaf(DerivativeOp::TangentArgument,1);
        if(i.op=="+") r.root=node(DerivativeOp::Add,{da,db});
        else if(i.op=="-") r.root=node(DerivativeOp::Subtract,{da,db});
        else if(i.op=="*") {
            const auto a=leaf(DerivativeOp::PrimalArgument),b=leaf(DerivativeOp::PrimalArgument,1);
            const auto left=node(DerivativeOp::Multiply,{da,b}),right=node(DerivativeOp::Multiply,{a,db});
            r.root=node(DerivativeOp::Add,{left,right});
        } else if(i.op=="/") {
            // d(a/b)=(da-(a/b)*db)/b；使用已有原值结果，避免 b*b 溢出。
            const auto y=leaf(DerivativeOp::PrimalResult),b=leaf(DerivativeOp::PrimalArgument,1);
            const auto scaled=node(DerivativeOp::Multiply,{y,db});
            const auto numerator=node(DerivativeOp::Subtract,{da,scaled});
            r.root=node(DerivativeOp::Divide,{numerator,b});
        }
    }
    return r;
}
std::string_view kind_name(DerivativeKind kind) {
    switch(kind) {
    case DerivativeKind::Zero:return "zero";case DerivativeKind::Undefined:return "undef";
    case DerivativeKind::Phi:return "phi";case DerivativeKind::Parameter:return "parameter_seed";
    case DerivativeKind::Expression:return "expression";case DerivativeKind::Discrete:return "discrete_guard";
    case DerivativeKind::UnaryMath:return "chain_unary";case DerivativeKind::BinaryMath:return "chain_binary";
    case DerivativeKind::Power:return "chain_power";case DerivativeKind::Extremum:return "select_extremum";
    }
    return "invalid";
}
std::string_view op_name(DerivativeOp op) {
    switch(op) {
    case DerivativeOp::Zero:return "zero";case DerivativeOp::PrimalArgument:return "primal_arg";
    case DerivativeOp::TangentArgument:return "tangent_arg";case DerivativeOp::PrimalResult:return "primal_result";
    case DerivativeOp::Negate:return "neg";case DerivativeOp::Add:return "add";case DerivativeOp::Subtract:return "sub";
    case DerivativeOp::Multiply:return "mul";case DerivativeOp::Divide:return "div";
    }
    return "invalid";
}
}
DerivativeIr lower_derivatives(const mir::MirFunction& f) {
    DerivativeIr ir;ir.diagnostics=M::verify_mir(f);
    if(ir.has_errors()) return ir;
    if(f.form!=M::MirForm::Ssa) {
        ir.diagnostics.push_back({Severity::Error,"AD_IR","derivative lowering requires SSA",{},{}});
        return ir;
    }
    ir.rules.resize(f.values.size());
    for(const auto& b:f.blocks) for(const auto& i:b.instructions) if(i.result)
        ir.rules[i.result->value]=rule(i,f.values[i.result->value].type);
    ir.diagnostics=verify_derivatives(f,ir);return ir;
}
std::vector<Diagnostic> verify_derivatives(const mir::MirFunction& f,const DerivativeIr& ir) {
    auto ds=M::verify_mir(f);if(!ds.empty()) return ds;
    auto fail=[&](std::string message){ds.push_back({Severity::Error,"AD_IR",std::move(message),{},{}});};
    if(f.form!=M::MirForm::Ssa||ir.rules.size()!=f.values.size()) {fail("derivative rule arena does not match SSA");return ds;}
    for(const auto& b:f.blocks) for(const auto& i:b.instructions) if(i.result) {
        const auto& r=ir.rules[i.result->value];
        if(r.result!=*i.result||r.source!=i.opcode||r.type!=f.values[i.result->value].type||
           r.arguments!=i.operands||r.operation!=i.op||r.kind!=classify(i,r.type))
            fail("derivative definition does not match its primal instruction");
        if(r.kind!=DerivativeKind::Expression) {
            if(!r.nodes.empty()||r.root) fail("non-expression rule contains a local expression DAG");
            continue;
        }
        if(!r.root||*r.root>=r.nodes.size()) fail("missing or invalid derivative expression root");
        for(std::size_t id=0;id<r.nodes.size();++id) {
            const auto& n=r.nodes[id];
            const auto arity=n.opcode==DerivativeOp::Negate?1u:
                n.opcode==DerivativeOp::Add||n.opcode==DerivativeOp::Subtract||
                n.opcode==DerivativeOp::Multiply||n.opcode==DerivativeOp::Divide?2u:0u;
            if(n.inputs.size()!=arity) fail("wrong derivative node arity");
            for(const auto child:n.inputs) if(child>=id) fail("derivative fold order requires earlier child definitions");
            if((n.opcode==DerivativeOp::PrimalArgument||n.opcode==DerivativeOp::TangentArgument)&&n.argument>=r.arguments.size())
                fail("invalid derivative operand reference");
            if(op_name(n.opcode)=="invalid") fail("invalid derivative node opcode");
        }
    }
    return ds;
}
Jet evaluate_derivative(const DerivativeRule& r,const std::vector<Jet>& args,double primal,std::size_t lanes) {
    if(args.size()!=r.arguments.size()||lanes==0||lanes>64) throw Error("derivative IR argument shape mismatch");
    for(const auto& a:args) if(a.tangent.size()!=lanes) throw Error("derivative IR tangent shape mismatch");
    if(r.kind==DerivativeKind::Undefined||r.kind==DerivativeKind::Phi||r.kind==DerivativeKind::Parameter)
        throw Error("derivative rule requires parameter/CFG execution context");
    auto result=constant(primal,lanes);
    const auto active=std::any_of(args.begin(),args.end(),[](const auto& a){return a.active;});
    if(r.kind==DerivativeKind::Zero) return result;
    if(r.kind==DerivativeKind::Discrete) {
        if(active&&r.source==M::Opcode::Convert) throw Error("active Real to Integer conversion is not differentiable");
        if(active&&r.source==M::Opcode::Binary&&args.size()==2&&args[0].value==args[1].value&&r.arguments[0]!=r.arguments[1])
            throw Error("active comparison boundary requires a derivative policy");
        if(active&&r.source==M::Opcode::Unary&&args[0].value==0)
            throw Error("active Boolean boundary requires a derivative policy");
        return result;
    }
    if(!active) return result;
    result.active=true;
    if(r.kind==DerivativeKind::Expression) {
        if(!r.root) throw Error("derivative expression has no root");
        std::vector<double> nodes(r.nodes.size());
        for(std::size_t lane=0;lane<lanes;++lane) {
            for(std::size_t id=0;id<r.nodes.size();++id) {
                const auto& n=r.nodes[id];
                const auto input=[&](std::size_t k){return nodes.at(n.inputs.at(k));};
                double v=0;
                switch(n.opcode) {
                case DerivativeOp::Zero:break;
                case DerivativeOp::PrimalArgument:v=args.at(n.argument).value;break;
                case DerivativeOp::TangentArgument:v=args.at(n.argument).tangent.at(lane);break;
                case DerivativeOp::PrimalResult:v=primal;break;
                case DerivativeOp::Negate:v=-input(0);break;
                case DerivativeOp::Add:v=input(0)+input(1);break;
                case DerivativeOp::Subtract:v=input(0)-input(1);break;
                case DerivativeOp::Multiply:v=input(0)*input(1);break;
                case DerivativeOp::Divide:
                    if(input(1)==0) throw Error("division by zero in derivative IR");
                    v=input(0)/input(1);break;
                }
                if(!std::isfinite(v)) throw Error("non-finite derivative IR node");
                nodes[id]=v;
            }
            result.tangent[lane]=nodes.at(*r.root);
        }
    } else if(r.kind==DerivativeKind::UnaryMath) result=math(r.operation,args.at(0));
    else if(r.kind==DerivativeKind::BinaryMath) result=math_binary(r.operation,args.at(0),args.at(1));
    else if(r.kind==DerivativeKind::Power) result=binary("**",args.at(0),args.at(1));
    else if(r.kind==DerivativeKind::Extremum) {
        if(args[0].value==args[1].value&&r.arguments[0]!=r.arguments[1])
            throw Error("active min/max boundary requires a derivative policy");
        result=select_extremum(r.operation=="max",args[0],args[1]);
        const auto selected=(r.operation=="max"?args[0].value>=args[1].value:args[0].value<=args[1].value)?0u:1u;
        result.active=args[selected].active;
    } else throw Error("derivative rule requires parameter/CFG execution context");
    result.value=primal;return result;
}
void print_derivatives(std::ostream& out,const mir::MirFunction& f,const DerivativeIr& ir) {
    out<<"Derivative IR: vector tangents, local DAGs in verified fold order\n";
    for(std::size_t b=0;b<f.blocks.size();++b) {
        out<<"bb"<<b<<":\n";
        for(const auto& i:f.blocks[b].instructions) {
            if(!i.result) {
                if(i.opcode==M::Opcode::ContributeFlow) out<<"  d(flow) += d(v"<<i.operands[0].value<<") using primal endpoint orientation\n";
                continue;
            }
            const auto& r=ir.rules[i.result->value];
            out<<"  d(v"<<r.result.value<<") = "<<kind_name(r.kind);
            if(!r.operation.empty()) out<<' '<<r.operation;
            for(const auto arg:r.arguments) out<<" [v"<<arg.value<<", d(v"<<arg.value<<")]";
            if(i.parameter) out<<' '<<f.hir->bindings->symbol(*i.parameter).name;
            for(const auto& edge:i.incoming) out<<" [bb"<<edge.predecessor.value<<": d(v"<<edge.value.value<<")]";
            out<<'\n';
            for(std::size_t id=0;id<r.nodes.size();++id) {
                const auto& n=r.nodes[id];out<<"    q"<<id<<" = "<<op_name(n.opcode);
                if(n.opcode==DerivativeOp::PrimalArgument||n.opcode==DerivativeOp::TangentArgument)
                    out<<" v"<<r.arguments[n.argument].value;
                for(const auto child:n.inputs) out<<" q"<<child;
                out<<'\n';
            }
            if(r.root) out<<"    yield q"<<*r.root<<'\n';
        }
    }
}
} // namespace vagrad::ad
