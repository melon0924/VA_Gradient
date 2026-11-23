#include "vagrad/codegen/c.hpp"
#include "c_runtime.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <set>
#include <sstream>

namespace vagrad::codegen {
namespace {
namespace H=hir;namespace M=mir;namespace A=ad;
const std::vector<std::string> math_names{"min","max","abs","sqrt","exp","ln","log","sin","cos","tan","asin",
    "acos","atan","atan2","sinh","cosh","tanh","asinh","acosh","atanh","ceil","floor","$vt"};
std::string num(std::size_t x){return std::to_string(x);}
std::string quote(std::string_view text) {
    std::string out="\"";
    for(const unsigned char c:text) {
        if(c=='\"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}
        else if(c<32) {out+='\\';out+=static_cast<char>('0'+((c>>6)&7));out+=static_cast<char>('0'+((c>>3)&7));out+=static_cast<char>('0'+(c&7));}
        else out+=static_cast<char>(c);
    }
    return out+'\"';
}
std::pair<std::pair<std::size_t,std::size_t>,int> flow_key(const H::Probe& p) {
    const auto ground=std::numeric_limits<std::size_t>::max();
    auto a=p.positive?p.positive->value:ground,b=p.negative?p.negative->value:ground;
    return a>b?std::pair{std::pair{b,a},-1}:std::pair{std::pair{a,b},1};
}
class Emitter {
public:
    Emitter(const A::Prepared& p,const H::ParamEnv& e):prepared(p),env(e),f(p.function()),h(*f.hir),s(p.sparsity()) {}
    const A::Prepared& prepared;const H::ParamEnv& env;const M::MirFunction& f;const H::HirModule& h;const A::Sparsity& s;
    CSource result;std::ostringstream out;
    std::map<std::size_t,std::size_t> parameters,nets,variables;
    std::vector<std::string> net_names,output_names;
    std::vector<std::pair<std::size_t,const H::BranchInfo*>> flows;
    std::vector<std::size_t> offsets;
    void fail(H::Origin origin,std::string message) {
        result.diagnostics.push_back(H::semantic_diagnostic(*h.bindings,origin,"CGEN_UNSUPPORTED",std::move(message)));
    }
    std::string v(M::ValueId id) const {return "v["+num(id.value)+"]";}
    std::string flag(M::ValueId id) const {return "a["+num(id.value)+"]";}
    std::string d(M::ValueId id,std::size_t lane) const {
        const auto bit=A::LaneMask{1}<<lane;
        if(!(s.values[id.value]&bit)) return "0.0";
        const auto rank=static_cast<std::size_t>(std::popcount(s.values[id.value]&(bit-1)));
        return "d["+num(offsets[id.value]+rank)+"]";
    }
    int math_id(std::string_view name) const {
        const auto found=std::find(math_names.begin(),math_names.end(),name);
        return found==math_names.end()?-1:static_cast<int>(found-math_names.begin());
    }
    // C0 的范围表达式限字面量、参数引用、转换和正负号；其他形式明确拒绝。
    std::string bound_expr(H::ExprId id,std::size_t depth=0) {
        const auto& e=h.expr(id);
        if(depth>256) {fail(e.origin,"C0 bound expression depth limit");return "NAN";}
        if(e.kind==H::ExprKind::Literal&&H::numeric(e.type)) return H::scalar_text(e.literal);
        if(e.kind==H::ExprKind::Ref&&parameters.contains(e.symbol->value)) return "p["+num(parameters.at(e.symbol->value))+"]";
        if(e.kind==H::ExprKind::Convert) {
            const auto x=bound_expr(e.operands[0],depth+1);
            return e.type==H::Type::Integer?"vag_integer("+x+",1)":x;
        }
        if(e.kind==H::ExprKind::Unary&&(e.op=="+"||e.op=="-")) return "("+e.op+bound_expr(e.operands[0],depth+1)+")";
        fail(e.origin,"C0 parameter bounds support literals, parameter refs, conversion and signs");return "NAN";
    }
    std::string bound(const H::Bound& b) {
        if(b.kind==H::Bound::Kind::PositiveInfinity) return "INFINITY";
        if(b.kind==H::Bound::Kind::NegativeInfinity) return "(-INFINITY)";
        return bound_expr(*b.expression);
    }
    bool profile() {
        if(prepared.has_errors()) {result.diagnostics=prepared.diagnostics();return false;}
        if(prepared.plan().mode!=A::ParameterMode::Effective) {
            fail(h.parameters.front().origin,"C0 supports independent effective parameters only");return false;
        }
        result.diagnostics=H::validate_effective_parameters(h,env);
        if(result.has_errors()) return false;
        for(std::size_t k=0;k<h.parameters.size();++k) {
            const auto& p=h.parameters[k];parameters[p.symbol.value]=k;
            if(!H::numeric(p.type)) fail(p.origin,"C0 supports numeric effective parameters only");
        }
        for(const auto& symbol:h.bindings->symbols) if(symbol.kind==H::SymbolKind::Net) {
            const auto id=static_cast<std::size_t>(&symbol-h.bindings->symbols.data());
            nets[id]=net_names.size();net_names.push_back(symbol.name);
        }
        for(const auto& variable:h.variables) if(h.bindings->symbol(variable.symbol).scope==h.scope&&
            (!f.output_projection||std::find(f.output_projection->begin(),f.output_projection->end(),variable.symbol)!=f.output_projection->end())) {
            variables[variable.symbol.value]=output_names.size();output_names.push_back(h.bindings->symbol(variable.symbol).name);
        }
        for(const auto& b:h.branches) if(!f.flow_projection||std::find(f.flow_projection->begin(),f.flow_projection->end(),b.symbol)!=f.flow_projection->end()) {
            flows.push_back({output_names.size(),&b});output_names.push_back("flow:"+h.bindings->symbol(b.symbol).name);
        }
        const std::set<std::string> binary{"+","-","*","/","%","**","==","!=","<","<=",">",">=","&&","||"};
        for(const auto& b:f.blocks) for(const auto& i:b.instructions) {
            if(i.result&&!H::numeric(f.values[i.result->value].type)) fail(i.origin,"C0 does not support String/Void values");
            if(i.opcode==M::Opcode::DiagnosticCall||i.opcode==M::Opcode::LoadLocal||i.opcode==M::Opcode::StoreLocal)
                fail(i.origin,"C0 does not support diagnostics or mutable local instructions");
            if(i.opcode==M::Opcode::Binary&&!binary.contains(i.op)) fail(i.origin,"C0 unsupported binary operation: "+i.op);
            if(i.opcode==M::Opcode::Unary&&i.op!="+"&&i.op!="-"&&i.op!="!") fail(i.origin,"C0 unsupported unary operation: "+i.op);
            if(i.opcode==M::Opcode::MathCall&&math_id(i.op)<0) fail(i.origin,"C0 unsupported math operation: "+i.op);
        }
        return !result.has_errors();
    }
    void metadata() {
        auto strings=[&](std::string_view name,const std::vector<std::string>& values) {
            out<<"static const char* const "<<name<<"[]={";
            if(values.empty()) out<<"\"\"";
            for(std::size_t k=0;k<values.size();++k) out<<(k?",":"")<<quote(values[k]);
            out<<"};\n";
        };
        std::vector<std::string> param_names,seed_names=prepared.seeds();
        for(const auto& p:h.parameters) param_names.push_back(h.bindings->symbol(p.symbol).name);
        strings("param_names",param_names);strings("node_names",net_names);strings("result_names",output_names);strings("seed_names",seed_names);
        out<<"static const double defaults[]={";
        if(h.parameters.empty()) out<<"0";
        for(std::size_t k=0;k<h.parameters.size();++k) out<<(k?",":"")<<H::scalar_text(*env.values[h.parameters[k].symbol.value]);
        out<<"};\n";
        out<<"VAG_EXPORT int vagrad_parameter_overridable(size_t i){switch(i){";
        for(std::size_t k=0;k<h.parameters.size();++k) out<<"case "<<k<<":return "<<h.parameters[k].overridable<<";";
        out<<"default:return 0;}}\nVAG_EXPORT int vagrad_parameter_type(size_t i){switch(i){";
        for(std::size_t k=0;k<h.parameters.size();++k) out<<"case "<<k<<":return "<<(h.parameters[k].type==H::Type::Integer?1:0)<<";";
        out<<"default:return -1;}}\n";
        for(const auto& spec:std::vector<std::pair<std::string,std::size_t>>{{"parameter",h.parameters.size()},{"node",net_names.size()},
                {"output",output_names.size()},{"seed",seed_names.size()}})
            out<<"VAG_EXPORT size_t vagrad_"<<spec.first<<"_count(void){return "<<spec.second<<";}\n";
        out<<"VAG_EXPORT const char* vagrad_parameter_name(size_t i){return i<"<<h.parameters.size()<<"?param_names[i]:NULL;}\n"
           <<"VAG_EXPORT const char* vagrad_node_name(size_t i){return i<"<<net_names.size()<<"?node_names[i]:NULL;}\n"
           <<"VAG_EXPORT const char* vagrad_output_name(size_t i){return i<"<<output_names.size()<<"?result_names[i]:NULL;}\n"
           <<"VAG_EXPORT const char* vagrad_seed_name(size_t i){return i<"<<seed_names.size()<<"?seed_names[i]:NULL;}\n"
           <<"VAG_EXPORT double vagrad_default_parameter(size_t i){return i<"<<h.parameters.size()<<"?defaults[i]:NAN;}\n";
    }
    void constraints() {
        for(std::size_t k=0;k<h.parameters.size();++k) {
            const auto& p=h.parameters[k];
            out<<"token="<<p.origin.token_index<<"; if(!isfinite(p["<<k<<"])) FAIL(VAG_PARAMETER);\n";
            if(p.type==H::Type::Integer) out<<"if(p["<<k<<"]!=vag_integer(p["<<k<<"],0)) FAIL(VAG_PARAMETER);\n";
            if(p.constraints.empty()) continue;
            out<<"{int any=0,match=0,excluded=0; double low,high; int found;\n";
            for(const auto& c:p.constraints) {
                out<<"low="<<bound(c.lower)<<";high="<<(c.interval?bound(c.upper):"low")<<";\n";
                if(c.lower.kind==H::Bound::Kind::Expression) out<<"if(!isfinite(low)) FAIL(VAG_PARAMETER);\n";
                if(c.interval&&c.upper.kind==H::Bound::Kind::Expression) out<<"if(!isfinite(high)) FAIL(VAG_PARAMETER);\n";
                out<<"if(low>high";
                if(c.interval&&(!c.lower_inclusive||!c.upper_inclusive)) out<<"||low==high";
                out<<") FAIL(VAG_PARAMETER);\n";
                if(c.interval) out<<"found=p["<<k<<"]"<<(c.lower_inclusive?">=":">")<<"low&&p["<<k<<"]"<<(c.upper_inclusive?"<=":"<")<<"high;\n";
                else out<<"found=p["<<k<<"]==low;\n";
                out<<(c.exclude?"excluded=excluded||found;\n":"any=1;match=match||found;\n");
            }
            out<<"if((any&&!match)||excluded) FAIL(VAG_PARAMETER);}\n";
        }
    }
    void gradients(const M::Instruction& i) {
        if(!i.result) return;
        const auto id=*i.result;const auto& r=prepared.derivatives().rules[id.value];
        std::string active="0";for(const auto arg:i.operands) active+="||"+flag(arg);
        out<<flag(id)<<"=("<<active<<");\n";
        if(r.kind==A::DerivativeKind::Zero||r.kind==A::DerivativeKind::Discrete) {
            if(r.kind==A::DerivativeKind::Discrete) {
                if(i.opcode==M::Opcode::Convert) out<<"if("<<flag(id)<<") FAIL(VAG_BOUNDARY);\n";
                else if(i.opcode==M::Opcode::Unary) out<<"if("<<flag(id)<<"&&"<<v(i.operands[0])<<"==0) FAIL(VAG_BOUNDARY);\n";
                else if(i.operands.size()==2&&i.operands[0]!=i.operands[1])
                    out<<"if("<<flag(id)<<"&&"<<v(i.operands[0])<<"=="<<v(i.operands[1])<<") FAIL(VAG_BOUNDARY);\n";
            }
            out<<flag(id)<<"=0;\n";
        } else if(r.kind==A::DerivativeKind::Parameter) out<<flag(id)<<"="<<(s.values[id.value]!=0)<<";\n";
        else if(r.kind==A::DerivativeKind::Extremum) {
            if(i.operands[0]!=i.operands[1]) out<<"if("<<flag(id)<<"&&"<<v(i.operands[0])<<"=="<<v(i.operands[1])<<") FAIL(VAG_BOUNDARY);\n";
        }
        for(std::size_t lane=0;lane<s.lanes;++lane) if(s.values[id.value]&(A::LaneMask{1}<<lane)) {
            const auto target=d(id,lane);
            if(r.kind==A::DerivativeKind::Parameter) out<<target<<"="<<(i.parameter==prepared.plan().parameters[lane]?1:0)<<";\n";
            else if(r.kind==A::DerivativeKind::Expression) {
                out<<"if("<<flag(id)<<"){\n";
                for(std::size_t q=0;q<r.nodes.size();++q) {
                    const auto& n=r.nodes[q];std::string expression="0.0";
                    if(n.opcode==A::DerivativeOp::PrimalArgument) expression=v(i.operands[n.argument]);
                    else if(n.opcode==A::DerivativeOp::TangentArgument) expression=d(i.operands[n.argument],lane);
                    else if(n.opcode==A::DerivativeOp::PrimalResult) expression=v(id);
                    else if(n.opcode==A::DerivativeOp::Negate) expression="-q"+num(n.inputs[0]);
                    else if(n.inputs.size()==2) {
                        const auto op=n.opcode==A::DerivativeOp::Add?"+":n.opcode==A::DerivativeOp::Subtract?"-":n.opcode==A::DerivativeOp::Multiply?"*":"/";
                        expression="q"+num(n.inputs[0])+op+"q"+num(n.inputs[1]);
                    }
                    out<<"double q"<<q<<"="<<expression<<";if(!isfinite(q"<<q<<")) FAIL(VAG_DOMAIN);\n";
                }
                out<<target<<"=q"<<*r.root<<";}else "<<target<<"=0;\n";
            } else if(r.kind==A::DerivativeKind::UnaryMath||r.kind==A::DerivativeKind::BinaryMath||
                      r.kind==A::DerivativeKind::Power||r.kind==A::DerivativeKind::Extremum) {
                const auto x=i.operands[0];const bool two=i.operands.size()==2;
                out<<target<<"=vag_chain("<<(r.kind==A::DerivativeKind::Power?23:math_id(i.op))<<','<<v(x)<<','
                   <<(two?v(i.operands[1]):"0")<<','<<v(id)<<','<<d(x,lane)<<','
                   <<(two?d(i.operands[1],lane):"0")<<','<<flag(x)<<','<<(two?flag(i.operands[1]):"0")<<",&status);\n"
                   <<"if(status) goto done;\n";
            } else out<<target<<"=0;\n";
        }
        if(r.kind==A::DerivativeKind::Extremum)
            out<<flag(id)<<"=("<<v(i.operands[0])<<(i.op=="max"?">=":"<=")<<v(i.operands[1])<<")?"
               <<flag(i.operands[0])<<':'<<flag(i.operands[1])<<";\n";
        if(r.kind==A::DerivativeKind::UnaryMath&&(i.op=="ceil"||i.op=="floor")) out<<flag(id)<<"=0;\n";
    }
    void contribution(const M::Instruction& i) {
        const auto [source,sign]=flow_key(*i.probe);if(source.first==source.second) return;
        for(const auto& [row,branch]:flows) {
            const auto [target,direction]=flow_key(branch->probe);if(target!=source) continue;
            const auto prefix=sign*direction<0?"-":"";
            out<<"out["<<row<<"]+="<<prefix<<v(i.operands[0])<<";if(!isfinite(out["<<row<<"])) FAIL(VAG_DOMAIN);\n";
            for(std::size_t lane=0;lane<s.lanes;++lane)
                out<<"jac["<<row*s.lanes+lane<<"]+="<<prefix<<d(i.operands[0],lane)<<";if(!isfinite(jac["<<row*s.lanes+lane<<"])) FAIL(VAG_DOMAIN);\n";
        }
    }
    void instruction(const M::Instruction& i) {
        out<<"{token="<<i.origin.token_index<<";TICK();\n";
        for(const auto arg:i.operands) out<<"if(!e["<<arg.value<<"]||!defined["<<arg.value<<"]) FAIL(VAG_UNASSIGNED);\n";
        if(i.opcode==M::Opcode::ContributeFlow) contribution(i);
        if(!i.result) {out<<"}\n";return;}
        const auto id=*i.result;const auto type=f.values[id.value].type;
        if(i.opcode==M::Opcode::Undef) {
            out<<"e["<<id.value<<"]=1;defined["<<id.value<<"]=0;"<<flag(id)<<"=0;}\n";return;
        }
        std::string expression="0";const auto arg=[&](std::size_t k){return v(i.operands.at(k));};
        switch(i.opcode) {
        case M::Opcode::Constant:expression=H::scalar_text(i.literal);break;
        case M::Opcode::LoadParam:expression="p["+num(parameters.at(i.parameter->value))+"]";break;
        case M::Opcode::Convert:expression=type==H::Type::Integer?"vag_integer("+arg(0)+",1)":arg(0);break;
        case M::Opcode::Unary:expression="("+i.op+arg(0)+")";break;
        case M::Opcode::Binary:
            if(i.op=="/"||i.op=="%") out<<"if("<<arg(1)<<"==0) FAIL(VAG_DOMAIN);\n";
            expression=i.op=="**"?"pow("+arg(0)+","+arg(1)+")":i.op=="%"?"fmod("+arg(0)+","+arg(1)+")":"("+arg(0)+i.op+arg(1)+")";
            if(type==H::Type::Integer) expression="vag_integer("+expression+",0)";
            break;
        case M::Opcode::Temperature:out<<"if(!isfinite(temperature)||temperature<=0) FAIL(VAG_INPUT);\n";expression="temperature";break;
        case M::Opcode::ReadPotential: {
            auto voltage=[&](std::optional<H::SymbolId> symbol) {
                if(!symbol) return std::string("0.0");
                const auto name="voltages["+num(nets.at(symbol->value))+"]";
                out<<"if(!voltages||!isfinite("<<name<<")) FAIL(VAG_INPUT);\n";return name;
            };
            const auto positive=voltage(i.probe->positive),negative=voltage(i.probe->negative);
            expression="("+positive+"-"+negative+")";break;
        }
        case M::Opcode::MathCall:
            if(i.operands.empty()) out<<"if(!isfinite(temperature)||temperature<=0) FAIL(VAG_INPUT);\n";
            expression="vag_math("+std::to_string(math_id(i.op))+","+(i.operands.empty()?"temperature":arg(0))+","+(i.operands.size()>1?arg(1):"0")+")";break;
        default:break;
        }
        out<<v(id)<<"="<<expression<<";if(!isfinite("<<v(id)<<")) FAIL(VAG_DOMAIN);\n"
           <<"e["<<id.value<<"]=defined["<<id.value<<"]=1;\n";
        gradients(i);out<<"}\n";
    }
    CSource run() {
        if(!profile()) return result;
        offsets.resize(f.values.size());std::size_t count=0,max_phi=0;
        for(std::size_t id=0;id<s.values.size();++id){offsets[id]=count;count+=static_cast<std::size_t>(std::popcount(s.values[id]));}
        for(const auto& b:f.blocks) max_phi=std::max(max_phi,static_cast<std::size_t>(std::count_if(b.instructions.begin(),b.instructions.end(),[](const auto& i){return i.opcode==M::Opcode::Phi;})));
        out<<c_runtime;metadata();
        out<<"VAG_EXPORT int vagrad_eval(const double* parameters,const double* voltages,double temperature,size_t limit,"
              "double* out,double* jac,uint8_t* valid,size_t* error_token){\n"
              "int status=0;size_t token=0,steps=0,k,previous=(size_t)-1;const double* p=parameters?parameters:defaults;\n"
              "double *v=NULL,*d=NULL,*pv=NULL,*pd=NULL;uint8_t *e=NULL,*defined=NULL,*a=NULL,*pdefined=NULL,*pa=NULL;\n"
              "#define FAIL(code) do{status=(code);goto done;}while(0)\n"
              "#define TICK() do{if(steps++>=limit)FAIL(VAG_LIMIT);}while(0)\n";
        if(!output_names.empty()) out<<"if(!out||!jac||!valid) return VAG_ARGUMENT;\n";
        out<<"for(k=0;k<"<<output_names.size()<<";++k){out[k]=NAN;valid[k]=0;}\n"
           <<"for(k=0;k<"<<output_names.size()*s.lanes<<";++k)jac[k]=NAN;\n";
        for(const auto& [row,b]:flows) {
            (void)b;out<<"out["<<row<<"]=0;valid["<<row<<"]=1;\n";
            for(std::size_t lane=0;lane<s.lanes;++lane) out<<"jac["<<row*s.lanes+lane<<"]=0;\n";
        }
        const auto n=std::max<std::size_t>(1,f.values.size()),np=std::max<std::size_t>(1,max_phi);
        out<<"v=calloc("<<n<<",sizeof(double));d=calloc("<<std::max<std::size_t>(1,count)<<",sizeof(double));\n"
           <<"e=calloc("<<n<<",1);defined=calloc("<<n<<",1);a=calloc("<<n<<",1);\n"
           <<"pv=calloc("<<np<<",sizeof(double));pd=calloc("<<std::max<std::size_t>(1,np*s.lanes)<<",sizeof(double));\n"
           <<"pdefined=calloc("<<np<<",1);pa=calloc("<<np<<",1);\n"
              "if(!v||!d||!e||!defined||!a||!pv||!pd||!pdefined||!pa)FAIL(VAG_MEMORY);\n";
        constraints();
        out<<"if(!isnan(temperature)&&(!isfinite(temperature)||temperature<=0))FAIL(VAG_INPUT);\n";
        for(std::size_t k=0;k<net_names.size();++k)
            out<<"if(voltages&&!isnan(voltages["<<k<<"])&&!isfinite(voltages["<<k<<"]))FAIL(VAG_INPUT);\n";
        out<<"goto bb"<<f.entry->value<<";\n";
        for(std::size_t b=0;b<f.blocks.size();++b) {
            const auto& block=f.blocks[b];out<<"bb"<<b<<":{\n";std::size_t phi=0;
            for(const auto& i:block.instructions) if(i.opcode==M::Opcode::Phi) {
                out<<"token="<<i.origin.token_index<<";TICK();\n";
                for(std::size_t edge=0;edge<i.incoming.size();++edge) {
                    const auto input=i.incoming[edge];
                    out<<(edge?"else ":"")<<"if(previous=="<<input.predecessor.value<<"){\n"
                       <<"if(!e["<<input.value.value<<"])FAIL(VAG_UNASSIGNED);\n"
                       <<"pv["<<phi<<"]="<<v(input.value)<<";pdefined["<<phi<<"]=defined["<<input.value.value<<"];pa["<<phi<<"]="<<flag(input.value)<<";\n";
                    for(std::size_t lane=0;lane<s.lanes;++lane) out<<"pd["<<phi*s.lanes+lane<<"]="<<d(input.value,lane)<<";\n";
                    out<<"}\n";
                }
                out<<"else FAIL(VAG_UNASSIGNED);\n";++phi;
            }
            phi=0;
            for(const auto& i:block.instructions) if(i.opcode==M::Opcode::Phi) {
                const auto id=*i.result;
                out<<v(id)<<"=pv["<<phi<<"];defined["<<id.value<<"]=pdefined["<<phi<<"];e["<<id.value<<"]=1;"<<flag(id)<<"=pa["<<phi<<"];\n";
                for(std::size_t lane=0;lane<s.lanes;++lane) if(s.values[id.value]&(A::LaneMask{1}<<lane)) out<<d(id,lane)<<"=pd["<<phi*s.lanes+lane<<"];\n";
                ++phi;
            }
            for(const auto& i:block.instructions) if(i.opcode!=M::Opcode::Phi) instruction(i);
            const auto& t=*block.terminator;out<<"token="<<t.origin.token_index<<";TICK();\n";
            if(t.kind==M::TerminatorKind::Jump) out<<"previous="<<b<<";goto bb"<<t.first->value<<";\n";
            else if(t.kind==M::TerminatorKind::Branch) {
                out<<"if(!e["<<t.condition->value<<"]||!defined["<<t.condition->value<<"])FAIL(VAG_UNASSIGNED);\n"
                   <<"if("<<flag(*t.condition)<<"&&"<<v(*t.condition)<<"==0)FAIL(VAG_BOUNDARY);\n"
                   <<"previous="<<b<<";if("<<v(*t.condition)<<"!=0)goto bb"<<t.first->value<<";else goto bb"<<t.second->value<<";\n";
            } else if(t.kind==M::TerminatorKind::Return) {
                for(const auto& output:t.outputs) if(variables.contains(output.symbol.value)) {
                    const auto row=variables.at(output.symbol.value);
                    out<<"if(defined["<<output.value.value<<"]){out["<<row<<"]="<<v(output.value)<<";valid["<<row<<"]=1;\n";
                    for(std::size_t lane=0;lane<s.lanes;++lane) out<<"jac["<<row*s.lanes+lane<<"]="<<d(output.value,lane)<<";\n";
                    out<<"}\n";
                }
                out<<"goto done;\n";
            } else out<<"FAIL(VAG_DOMAIN);\n";
            out<<"}\n";
        }
        out<<"done:\nif(error_token)*error_token=token;\n"
              "if(status){for(k=0;k<"<<output_names.size()<<";++k){out[k]=NAN;valid[k]=0;}"
           <<"for(k=0;k<"<<output_names.size()*s.lanes<<";++k)jac[k]=NAN;}\n"
              "free(v);free(d);free(e);free(defined);free(a);free(pv);free(pd);free(pdefined);free(pa);\n"
              "return status;\n#undef FAIL\n#undef TICK\n}\n";
        out<<"VAG_EXPORT int vagrad_eval_batch(size_t n,const double* parameters,const double* voltages,"
              "const double* temperatures,size_t limit,double* out,double* jac,uint8_t* valid,int* statuses,size_t* tokens){\n"
              "size_t row;if(n==0)return VAG_OK;if(!statuses)return VAG_ARGUMENT;\n";
        if(!output_names.empty()) out<<"if(!out||!jac||!valid)return VAG_ARGUMENT;\n";
        const auto largest=std::max({std::size_t{1},h.parameters.size(),net_names.size(),output_names.size()*s.lanes});
        out<<"if(n>SIZE_MAX/(sizeof(double)*"<<largest<<"))return VAG_ARGUMENT;\n"
              "for(row=0;row<n;++row)statuses[row]=vagrad_eval(parameters?parameters+row*"<<h.parameters.size()<<":NULL,"
           <<"voltages?voltages+row*"<<net_names.size()<<":NULL,temperatures?temperatures[row]:NAN,limit,"
           <<(output_names.empty()?"out":"out+row*"+num(output_names.size()))<<','
           <<(output_names.empty()?"jac":"jac+row*"+num(output_names.size()*s.lanes))<<','
           <<(output_names.empty()?"valid":"valid+row*"+num(output_names.size()))<<",tokens?tokens+row:NULL);\nreturn VAG_OK;\n}\n";
        if(!result.has_errors()) result.text=out.str();return result;
    }
};
}
CSource emit_c(const ad::Prepared& prepared,const hir::ParamEnv& defaults) {return Emitter(prepared,defaults).run();}
} // namespace vagrad::codegen
