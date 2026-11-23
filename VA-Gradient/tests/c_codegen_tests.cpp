#include "vagrad/codegen/c.hpp"
#include "mir_test_support.hpp"
#include <iostream>
#include <limits>

extern "C" {
std::size_t vagrad_parameter_count(void);
std::size_t vagrad_node_count(void);
std::size_t vagrad_output_count(void);
std::size_t vagrad_seed_count(void);
const char* vagrad_parameter_name(std::size_t);
const char* vagrad_node_name(std::size_t);
const char* vagrad_output_name(std::size_t);
const char* vagrad_seed_name(std::size_t);
double vagrad_default_parameter(std::size_t);
int vagrad_eval(const double*,const double*,double,std::size_t,double*,double*,std::uint8_t*,std::size_t*);
int vagrad_eval_batch(std::size_t,const double*,const double*,const double*,std::size_t,double*,double*,std::uint8_t*,int*,std::size_t*);
}
namespace {
using namespace mir_test;namespace A=vagrad::ad;namespace C=vagrad::codegen;
M::MirFunction fixture() {
    SourceManager sources;return M::promote_to_ssa(M::lower_mir(lower(sources,sources.load(VAGRAD_CGEN_FILE))));
}
void numerical() {
    const auto f=fixture();
    std::vector<std::string> seeds;
    for(std::size_t k=0;k<vagrad_seed_count();++k) seeds.emplace_back(vagrad_seed_name(k));
    const auto base=H::resolve_parameters(*f.hir);
    const auto compiled=A::prepare(f,base,seeds);
    const auto source=C::emit_c(compiled,base);
    require(!source.has_errors()&&source.text.find("VAG_EXPORT int vagrad_eval")!=std::string::npos,"C generator library");
    std::vector<double> p(vagrad_parameter_count()),nodes(vagrad_node_count()),values(vagrad_output_count()),jac(values.size()*seeds.size());
    std::vector<std::uint8_t> valid(values.size());std::size_t token=0;
#ifdef VAGRAD_CGEN_MATH
    const std::vector<double> sweep{-.4,.1,.4,.7};
#else
    const std::vector<double> sweep{.2,.7,1.,1.2};
#endif
    for(const auto x:sweep) for(const double drain:{.01,.05,.6}) for(const double t:{300.,600.}) {
        H::EvalInputs bias;bias.temperature_kelvin=t;
#ifdef VAGRAD_CGEN_MATH
        const auto env=H::resolve_parameters(*f.hir,{{"P",x}});
#elif defined(VAGRAD_CGEN_CONTROL)
        const auto env=H::resolve_parameters(*f.hir,{{"P",x},{"EN",std::int32_t{x<.7?0:1}}});
#else
        bias.node_voltages={{"g",x},{"d",drain},{"s",0}};
        const auto env=base;
#endif
        (void)drain;
        for(std::size_t k=0;k<p.size();++k) for(const auto& info:f.hir->parameters)
            if(f.hir->bindings->symbol(info.symbol).name==vagrad_parameter_name(k)) p[k]=H::number(*env.values[info.symbol.value]);
        for(std::size_t k=0;k<nodes.size();++k) nodes[k]=bias.node_voltages.at(vagrad_node_name(k));
        const auto reference=A::evaluate(compiled,env,bias);
        const auto status=vagrad_eval(p.data(),nodes.data(),t,100000,values.data(),jac.data(),valid.data(),&token);
        require(status==0&&!reference.has_errors(),"generated C executes interior point");
        for(std::size_t row=0;row<values.size();++row) {
            const std::string name=vagrad_output_name(row);
            if(!name.starts_with("flow:")&&!reference.primal.observations.contains(name)) {
                require(valid[row]==0&&std::isnan(values[row]),"unassigned C output remains invalid");
                continue;
            }
            require(valid[row]!=0,"assigned C output validity");
            if(name.starts_with("flow:")) {
                const auto branch=name.substr(5);close(values[row],reference.primal.branch_flows.at(branch),"C flow primal");
                for(std::size_t lane=0;lane<seeds.size();++lane) close(jac[row*seeds.size()+lane],reference.branch_flows.at(branch)[lane],"C flow gradient");
            } else {
                close(values[row],H::number(reference.primal.observations.at(name)),"C observation primal");
                for(std::size_t lane=0;lane<seeds.size();++lane) close(jac[row*seeds.size()+lane],reference.observations.at(name)[lane],"C observation gradient");
            }
        }
    }
    for(std::size_t k=0;k<p.size();++k) p[k]=vagrad_default_parameter(k);
    H::EvalInputs bias=inputs();
    for(std::size_t k=0;k<nodes.size();++k) nodes[k]=bias.node_voltages.at(vagrad_node_name(k));
    require(vagrad_eval(nullptr,nodes.data(),300,100000,values.data(),jac.data(),valid.data(),&token)==0,"C default parameter snapshot");
    require(vagrad_eval(p.data(),nodes.data(),300,1,values.data(),jac.data(),valid.data(),&token)==7,"C execution limit");
    require(std::all_of(valid.begin(),valid.end(),[](auto x){return x==0;})&&std::isnan(values[0]),"C error does not publish partial outputs");
    p[0]=-2;
    require(vagrad_eval(p.data(),nodes.data(),300,100000,values.data(),jac.data(),valid.data(),&token)==2,"C parameter constraint");
    p[0]=vagrad_default_parameter(0);
    require(vagrad_eval(p.data(),nodes.data(),0,100000,values.data(),jac.data(),valid.data(),&token)==3,"C missing/invalid temperature");
    if(!nodes.empty()) {
        nodes[0]=std::numeric_limits<double>::quiet_NaN();
        require(vagrad_eval(p.data(),nodes.data(),300,100000,values.data(),jac.data(),valid.data(),&token)==3,"C invalid node voltage");
    }
    require(vagrad_parameter_name(p.size())==nullptr&&std::isnan(vagrad_default_parameter(p.size())),"metadata bounds");
    for(std::size_t k=0;k<nodes.size();++k) nodes[k]=bias.node_voltages.at(vagrad_node_name(k));
    std::vector<double> batch_nodes;for(int k=0;k<3;++k) batch_nodes.insert(batch_nodes.end(),nodes.begin(),nodes.end());
    std::vector<double> batch_values(3*values.size()),batch_jac(3*jac.size());
    std::vector<std::uint8_t> batch_valid(3*valid.size());int statuses[3]={};std::size_t tokens[3]={};
    const double temperatures[3]={300,0,600};
    require(vagrad_eval_batch(3,nullptr,batch_nodes.data(),temperatures,100000,batch_values.data(),batch_jac.data(),batch_valid.data(),statuses,tokens)==0,
        "batch dispatch");
    require(statuses[0]==0&&statuses[1]==3&&statuses[2]==0,"C batch isolates failed row and continues");
    require(vagrad_eval_batch(0,nullptr,nullptr,nullptr,0,nullptr,nullptr,nullptr,nullptr,nullptr)==0,"empty C batch");
}
void capabilities() {
    const auto f=M::promote_to_ssa(text("module m; parameter real P=1 from (0:2*P); real ids; analog ids=P; endmodule"));
    const auto env=H::resolve_parameters(*f.hir);
    const auto rejected=C::emit_c(A::prepare(f,env,{"P"}),env);
    require(rejected.has_errors()&&rejected.text.empty(),"unsupported constraint never emits partial C");
    const auto plain=M::promote_to_ssa(text("module m; parameter real P=1; real ids; analog ids=P; endmodule"));
    const auto parameters=H::resolve_parameters(*plain.hir);
    require(C::emit_c(A::prepare(plain,parameters,{"P"},A::ParameterMode::Defaults),parameters).has_errors(),"C0 rejects default-chain mode");
}
}
int main() {
    try {numerical();capabilities();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"Generated C compiled as C11 and matched C++ primal/gradient references\n";
}
