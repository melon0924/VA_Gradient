#include "vagrad/ad/eval.hpp"
#include "vagrad/ad/ir.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <ostream>

namespace vagrad::ad {
namespace {
namespace M=mir;namespace H=hir;
using Key=std::pair<std::size_t,std::size_t>;
std::pair<Key,double> key(const H::Probe& p) {
    const auto ground=std::numeric_limits<std::size_t>::max();
    auto a=p.positive?p.positive->value:ground,b=p.negative?p.negative->value:ground;
    return a>b?std::pair{Key{b,a},-1.0}:std::pair{Key{a,b},1.0};
}
class Observer final:public M::ExecutionObserver {
public:
    Observer(const M::MirFunction& f,const Plan& p,const H::ParamEnv& e,Result& r,const std::vector<LaneMask>& masks,const DerivativeIr& ir)
        :f(f),plan(p),env(e),result(r),state(masks,p.parameters.size()),depends(f.values.size()),derivatives(ir) {}
    const M::MirFunction& f;const Plan& plan;const H::ParamEnv& env;Result& result;
    TangentState state;std::map<Key,std::vector<double>> flows;
    std::vector<bool> depends;
    std::map<std::size_t,Jet> parameter_cache;
    const DerivativeIr& derivatives;
    H::Origin current{};
    std::vector<double> zero() const {return std::vector<double>(plan.parameters.size());}
    void phi(const std::vector<M::Instruction>& instructions,M::BlockId pred) override {
        if(!instructions.empty()) current=instructions.front().origin;
        std::vector<std::pair<M::ValueId,bool>> flags;
        for(const auto& i:instructions) {
            if(i.opcode!=M::Opcode::Phi) break;
            const auto edge=std::find_if(i.incoming.begin(),i.incoming.end(),[&](const auto& x){return x.predecessor==pred;});
            if(edge!=i.incoming.end()) flags.push_back({*i.result,depends[edge->value.value]});
        }
        state.apply_phi(instructions,pred);
        for(const auto [id,flag]:flags) depends[id.value]=flag;
    }
    void instruction(const M::Instruction& i,const std::vector<H::Scalar>& args,
                     const std::optional<H::Scalar>& value) override {
        current=i.origin;
        std::vector<Jet> a;
        for(std::size_t k=0;k<args.size();++k) {
            if(H::numeric(H::scalar_type(args[k]))) {
                const auto& d=state.get(i.operands[k]);
                if(!d) throw Error("AD operand tangent unavailable");
                a.push_back({H::number(args[k]),*d,depends[i.operands[k].value]});
            } else a.push_back({0,zero()});
        }
        if(i.opcode==M::Opcode::ContributeFlow) {
            const auto [pair,sign]=key(*i.probe);
            if(pair.first!=pair.second) {
                auto& d=flows[pair];if(d.empty()) d=zero();
                for(std::size_t k=0;k<d.size();++k) {
                    d[k]+=sign*a[0].tangent[k];
                    if(!std::isfinite(d[k])) throw Error("non-finite flow derivative");
                }
            }
        }
        if(!i.result) return;
        if(!value) {state.set(*i.result,{});return;}
        auto d=zero();
        const auto type=f.values[i.result->value].type;
        bool dependency=false;
        if(i.opcode==M::Opcode::LoadParam&&H::numeric(type)) {
            if(!parameter_cache.contains(i.parameter->value)) parameter_cache[i.parameter->value]=parameter_jet(plan,env,*i.parameter);
            const auto& jet=parameter_cache.at(i.parameter->value);d=jet.tangent;dependency=jet.active;
        }
        else {
            const auto evaluated=evaluate_derivative(derivatives.rules[i.result->value],a,
                H::numeric(type)?H::number(*value):0,plan.parameters.size());
            d=evaluated.tangent;dependency=evaluated.active;
        }
        state.set(*i.result,std::move(d));
        depends[i.result->value]=dependency;
    }
    void returned(const M::Terminator& t,const std::vector<std::optional<H::Scalar>>& values) override {
        current=t.origin;
        for(const auto& o:t.outputs) if(values[o.value.value]&&H::numeric(H::scalar_type(*values[o.value.value])))
            result.observations[f.hir->bindings->symbol(o.symbol).name]=*state.get(o.value);
    }
    void branch(const M::Terminator& t,const H::Scalar& condition) override {
        current=t.origin;
        if(depends[t.condition->value]&&H::number(condition)==0)
            throw Error("active direct branch boundary requires a derivative policy");
    }
};
}
Result evaluate(const mir::MirFunction& f,const hir::ParamEnv& env,const hir::EvalInputs& inputs,const std::vector<std::string>& names,ParameterMode mode,Storage storage) {
    return evaluate(prepare(f,env,names,mode),env,inputs,storage);
}
Result evaluate(const Prepared& prepared,const hir::ParamEnv& env,const hir::EvalInputs& inputs,Storage storage) {
    const auto& f=prepared.function();const auto& plan=prepared.plan();const auto& names=prepared.seeds();
    Result r;r.seeds=names;r.mode=plan.mode;r.storage=storage;r.diagnostics=prepared.diagnostics();
    if(r.has_errors()) return r;
    if(!prepared.accepts(env)) {
        r.diagnostics.push_back({Severity::Error,"AD_ENV","parameter environment or frozen default overrides changed; prepare again",{},{}});
        return r;
    }
    const auto& derivatives=prepared.derivatives();
    const auto& sparsity=prepared.sparsity();
    r.dense_tangent_slots=sparsity.dense_slots;
    r.tangent_slots=storage==Storage::Sparse?sparsity.sparse_slots:sparsity.dense_slots;
    if(r.tangent_slots>8000000) {
        r.diagnostics.push_back({Severity::Error,"AD_LIMIT","AD tangent matrix too large",{},{}});return r;
    }
    auto masks=sparsity.values;
    if(storage==Storage::Dense) {
        const auto all=names.size()==64?~LaneMask{0}:(LaneMask{1}<<names.size())-1;
        std::fill(masks.begin(),masks.end(),all);
    }
    Observer observer(f,plan,env,r,masks,derivatives);
    try {
        r.primal=mir::evaluate_mir(prepared.executable(),env,inputs,&observer);
        if(!r.primal.has_errors()) for(const auto& b:f.hir->branches) {
            const auto name=f.hir->bindings->symbol(b.symbol).name;
            if(!r.primal.branch_flows.contains(name)) continue;
            const auto [pair,sign]=key(b.probe);auto d=observer.flows.contains(pair)?observer.flows.at(pair):observer.zero();
            for(auto& v:d) v*=sign;r.branch_flows[name]=std::move(d);
        }
    } catch(const ParameterError& e) {
        r.diagnostics.push_back(hir::semantic_diagnostic(*f.hir->bindings,e.origin,"AD_PARAMETER",e.what()));
    } catch(const Error& e) {
        r.diagnostics.push_back(hir::semantic_diagnostic(*f.hir->bindings,observer.current,"AD_RULE",e.what()));
    }
    if(r.has_errors()) {r.observations.clear();r.branch_flows.clear();}
    return r;
}
std::vector<Result> evaluate_batch(const Prepared& prepared,const std::vector<BatchCase>& cases,Storage storage) {
    std::vector<Result> results;results.reserve(cases.size());
    for(const auto& input:cases) results.push_back(evaluate(prepared,input.parameters,input.inputs,storage));
    return results;
}
void print_result(std::ostream& out,const Result& r) {
    out<<"Forward AD status="<<(r.has_errors()?"error":"ok")
       <<" parameters="<<(r.mode==ParameterMode::Effective?"effective":"defaults")<<'\n';
    if(r.has_errors()) {
        for(const auto& d:r.diagnostics) out<<d.code<<": "<<d.message<<'\n';
        for(const auto& d:r.primal.diagnostics) out<<d.code<<": "<<d.message<<'\n';
        return;
    }
    out<<"Tangent storage="<<(r.storage==Storage::Sparse?"sparse":"dense")
       <<" slots="<<r.tangent_slots<<'/'<<r.dense_tangent_slots<<'\n';
    hir::print_evaluation(out,r.primal,"AD primal");
    auto rows=[&](const auto& values,std::string_view prefix) {
        for(const auto& [name,gradient]:values) for(std::size_t k=0;k<gradient.size();++k)
            out<<"d("<<prefix<<name<<")/d("<<r.seeds[k]<<") = "<<hir::scalar_text(gradient[k])<<'\n';
    };
    rows(r.observations,"");rows(r.branch_flows,"flow:");
}
} // namespace vagrad::ad
