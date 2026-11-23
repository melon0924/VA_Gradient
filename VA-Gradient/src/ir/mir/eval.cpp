#include "vagrad/ir/mir/eval.hpp"
#include "vagrad/runtime/scalar.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace vagrad::mir {
namespace {
namespace H=hir;
using FlowKey=std::pair<std::size_t,std::size_t>;
constexpr auto ground=std::numeric_limits<std::size_t>::max();
std::pair<FlowKey,double> flow_key(const H::Probe& p) {
    auto a=p.positive?p.positive->value:ground,b=p.negative?p.negative->value:ground;
    return a>b?std::pair{FlowKey{b,a},-1.0}:std::pair{FlowKey{a,b},1.0};
}
H::EvalResult execute_verified(const MirFunction& f,const H::ParamEnv& parameters,const H::EvalInputs& inputs,ExecutionObserver* observer) {
    H::EvalResult result;
    const auto& h=*f.hir;
    runtime::ScalarOps ops(h,result.diagnostics);
    ops.inputs=&inputs;ops.limit=inputs.step_limit;
    const auto origin=f.blocks[f.entry->value].terminator->origin;
    std::vector<std::optional<H::Scalar>> locals(f.form==MirForm::Local?f.locals.size():0),values(f.values.size());
    std::vector<bool> executed(f.values.size());
    std::map<FlowKey,double> flows;
    std::vector<H::DiagnosticRecord> records;
    try {
        if(parameters.has_errors()||parameters.bindings!=h.bindings||parameters.values.size()!=h.bindings->symbols.size())
            ops.fail(origin,"EVAL_HIR_ERRORS","MIR execution requires a valid matching parameter environment");
        std::set<std::string> nets;
        for(const auto& s:h.bindings->symbols) if(s.kind==H::SymbolKind::Net) nets.insert(s.name);
        for(const auto& [name,value]:inputs.node_voltages) {
            if(!nets.contains(name)) ops.fail(origin,"EVAL_INPUT","unknown node voltage input: "+name);
            if(!std::isfinite(value)) ops.fail(origin,"EVAL_INPUT","node voltage must be finite: "+name);
        }
        if(inputs.temperature_kelvin) (void)ops.temperature(origin);
        auto read=[&](ValueId id,H::Origin use)->H::Scalar {
            if(!executed[id.value]) ops.fail(use,"EVAL_MIR","instruction operand has not executed");
            if(!values[id.value]) ops.fail(use,"EVAL_STATE","SSA value is unassigned on this path");
            return *values[id.value];
        };
        auto current=*f.entry;
        std::optional<BlockId> previous;
        bool returned=false;
        while(!returned) {
            const auto& b=f.blocks[current.value];
            // 所有 phi 同时读取前驱状态，再统一提交；循环中不能逐条覆盖旧 phi 值。
            std::vector<std::pair<ValueId,std::optional<H::Scalar>>> phi_values;
            for(const auto& i:b.instructions) {
                if(i.opcode!=Opcode::Phi) break;
                ops.tick(i.origin);
                if(!previous) ops.fail(i.origin,"EVAL_MIR","phi reached without predecessor");
                const auto input=std::find_if(i.incoming.begin(),i.incoming.end(),[&](const auto& x){
                    return x.predecessor==*previous;});
                if(input==i.incoming.end()||!executed[input->value.value])
                    ops.fail(i.origin,"EVAL_MIR","phi predecessor value has not executed");
                phi_values.push_back({*i.result,values[input->value.value]});
            }
            if(observer&&previous) observer->phi(b.instructions,*previous);
            for(auto& [id,value]:phi_values) {values[id.value]=std::move(value);executed[id.value]=true;}
            for(const auto& i:b.instructions) {
                if(i.opcode==Opcode::Phi) continue;
                ops.tick(i.origin);
                std::vector<H::Scalar> args;
                for(const auto id:i.operands) args.push_back(read(id,i.origin));
                const auto type=i.result?f.values[i.result->value].type:H::Type::Void;
                std::optional<H::Scalar> value=std::int32_t{0};
                switch(i.opcode) {
                case Opcode::Undef:value.reset();break;
                case Opcode::RequireDefined:break; // 参数读取已经执行了未赋值检查
                case Opcode::Phi:break; // 块首统一执行
                case Opcode::Constant:value=ops.finite(i.literal,i.origin);break;
                case Opcode::LoadParam:
                    if(!parameters.values[i.parameter->value]) ops.fail(i.origin,"EVAL_PARAMETER","missing effective parameter value");
                    value=ops.convert(*parameters.values[i.parameter->value],type,i.origin);break;
                case Opcode::LoadLocal:
                    if(!locals[i.local->value]) {
                        const auto symbol=f.locals[i.local->value].symbol;
                        ops.fail(i.origin,"EVAL_STATE","local read before assignment: "+
                            (symbol?h.bindings->symbol(*symbol).name:"<merge temporary>"));
                    }
                    value=*locals[i.local->value];break;
                case Opcode::StoreLocal:locals[i.local->value]=args[0];break;
                case Opcode::Convert:value=ops.convert(args[0],type,i.origin);break;
                case Opcode::Unary:value=ops.unary(i.op,type,args[0],i.origin);break;
                case Opcode::Binary:value=ops.binary(i.op,type,args[0],args[1],i.origin);break;
                case Opcode::Temperature:value=ops.temperature(i.origin);break;
                case Opcode::ReadPotential: {
                    const auto a=ops.voltage(i.probe->positive,i.origin);
                    const auto bvoltage=ops.voltage(i.probe->negative,i.origin);
                    value=ops.finite(a-bvoltage,i.origin);break;
                }
                case Opcode::MathCall:value=ops.math(i.op,args,i.origin);break;
                case Opcode::DiagnosticCall:records.push_back({i.op,std::move(args),i.origin});break;
                case Opcode::ContributeFlow: {
                    const auto [key,sign]=flow_key(*i.probe);
                    if(key.first!=key.second) flows[key]=H::number(ops.finite(flows[key]+sign*H::number(args[0]),i.origin));
                    break;
                }
                }
                if(observer) observer->instruction(i,args,value);
                if(i.result) {values[i.result->value]=std::move(value);executed[i.result->value]=true;}
            }
            const auto& t=*b.terminator;ops.tick(t.origin);
            switch(t.kind) {
            case TerminatorKind::Jump:previous=current;current=*t.first;break;
            case TerminatorKind::Branch: {
                const auto condition=read(*t.condition,t.origin);
                if(observer) observer->branch(t,condition);
                const auto next=H::number(condition)!=0?*t.first:*t.second;
                previous=current;current=next;break;
            }
            case TerminatorKind::Return:returned=true;break;
            case TerminatorKind::Unreachable:ops.fail(t.origin,"EVAL_MIR","execution entered Unreachable");
            }
        }
        if(observer) observer->returned(*f.blocks[current.value].terminator,values);
        if(f.form==MirForm::Ssa) {
            for(const auto& output:f.blocks[current.value].terminator->outputs) {
                if(!executed[output.value.value]) ops.fail(origin,"EVAL_MIR","SSA output has not executed");
                if(values[output.value.value]) result.observations[h.bindings->symbol(output.symbol).name]=*values[output.value.value];
            }
        } else for(std::size_t k=0;k<f.locals.size();++k) if(f.locals[k].symbol&&locals[k]) {
                const auto& s=h.bindings->symbol(*f.locals[k].symbol);
                if(s.scope==h.scope) result.observations[s.name]=*locals[k];
            }
        for(const auto& b:h.branches) {
            if(f.flow_projection&&std::find(f.flow_projection->begin(),f.flow_projection->end(),b.symbol)==f.flow_projection->end()) continue;
            const auto [key,sign]=flow_key(b.probe);
            const auto it=flows.find(key);
            result.branch_flows[h.bindings->symbol(b.symbol).name]=it==flows.end()?0.0:sign*it->second;
        }
        auto endpoint=[&](std::size_t id){return id==ground?std::string("0"):h.bindings->symbol({id}).name;};
        for(const auto& [key,value]:flows) result.endpoint_flows[endpoint(key.first)+","+endpoint(key.second)]=value;
    } catch(const runtime::Stop&) {}
    result.steps=ops.steps;result.records=std::move(records);
    return result;
}
} // namespace
Executable::Executable(const MirFunction& f):function_(std::make_shared<const MirFunction>(f)),diagnostics_(verify_mir(*function_)) {}
H::EvalResult evaluate_mir(const MirFunction& f,const H::ParamEnv& parameters,const H::EvalInputs& inputs,ExecutionObserver* observer) {
    H::EvalResult result;result.diagnostics=verify_mir(f);
    if(result.has_errors()) return result;
    return execute_verified(f,parameters,inputs,observer);
}
H::EvalResult evaluate_mir(const Executable& executable,const H::ParamEnv& parameters,const H::EvalInputs& inputs,ExecutionObserver* observer) {
    H::EvalResult result;result.diagnostics=executable.diagnostics();
    if(result.has_errors()) return result;
    return execute_verified(executable.function(),parameters,inputs,observer);
}
} // namespace vagrad::mir
