#include "mir_test_support.hpp"
#include <iostream>

namespace {
using namespace mir_test;
void compare(const H::EvalResult& a,const H::EvalResult& b) {
    require(a.has_errors()==b.has_errors(),"HIR/MIR status");
    if(a.has_errors()) {
        require(a.diagnostics.front().code==b.diagnostics.front().code,"HIR/MIR error category");
        require(a.observations.empty()&&b.observations.empty(),"failure has no successful observations");return;
    }
    require(a.observations.size()==b.observations.size()&&a.branch_flows.size()==b.branch_flows.size()&&
        a.endpoint_flows.size()==b.endpoint_flows.size()&&a.records.size()==b.records.size(),"same output keys");
    for(const auto& [key,v]:a.observations) {
        const auto& w=b.observations.at(key);
        require(H::scalar_type(v)==H::scalar_type(w),"observation types");
        if(H::scalar_type(v)==H::Type::String) require(v==w,"string output");
        else close(H::number(v),H::number(w),key);
    }
    for(const auto& [key,v]:a.branch_flows) close(v,b.branch_flows.at(key),key);
    for(const auto& [key,v]:a.endpoint_flows) close(v,b.endpoint_flows.at(key),key);
    for(std::size_t k=0;k<a.records.size();++k) {
        require(a.records[k].function==b.records[k].function&&a.records[k].arguments.size()==b.records[k].arguments.size()&&
            a.records[k].origin.token_index==b.records[k].origin.token_index,"diagnostic order/provenance");
        for(std::size_t j=0;j<a.records[k].arguments.size();++j) {
            const auto& v=a.records[k].arguments[j];const auto& w=b.records[k].arguments[j];
            if(H::scalar_type(v)==H::Type::String) require(v==w,"diagnostic format");
            else close(H::number(v),H::number(w),"diagnostic argument");
        }
    }
}
H::EvalResult align(const M::MirFunction& m,const H::ParamEnv& p,const H::EvalInputs& i) {
    const auto a=H::evaluate_hir(*m.hir,p,i),b=M::evaluate_mir(m,p,i);
    compare(a,b);return b;
}
void model() {
    const auto m=sample();const auto p=H::resolve_parameters(*m.hir);
    struct Case {double vg,vd,t;int enable;double ids,flow;};
    for(const auto& c:std::vector<Case>{{.3,.05,300,1,5e-14,5e-14},{1,.05,300,1,2.625e-5,2.625e-5},
        {1,.8,300,1,1.5125e-4,1.5125e-4},{1,.55,300,1,1.5125e-4,1.5125e-4},
        {1,.05,300,0,5e-14,5e-14},{1,.05,600,1,2.625e-5,1.3125e-5}}) {
        const auto env=c.enable?p:H::resolve_parameters(*m.hir,{{"ENABLE",std::int32_t{0}}});
        const auto r=align(m,env,inputs(c.vg,c.vd,c.t));
        require(!r.has_errors(),"model evaluation");close(H::number(r.observations.at("ids")),c.ids,"hand ids");
        close(r.branch_flows.at("ds"),c.flow,"hand flow");
    }
    // 网格覆盖各分支；不要求两种解释器的执行步数相同。
    for(double vg:{.0,.3,.45,.46,.8,1.2}) for(double vd:{.0,.01,.1,.8})
        (void)align(m,p,inputs(vg,vd,350));
    for(const auto& [name,value]:std::map<std::string,double>{{"G0",.002},{"VTH0",.6},{"TNOM",600},{"L",2e-6}})
        (void)align(m,H::resolve_parameters(*m.hir,{{name,value}}),inputs());
    PreprocessOptions opts;opts.defines={{"SOFT_THRESHOLD","1"},{"TRACE_MOS","1"}};
    const auto alt=sample(opts);
    require(align(alt,H::resolve_parameters(*alt.hir),inputs()).records.size()==1,"configured diagnostics");
    auto i=inputs();i.node_voltages.erase("g");require(align(m,p,i).has_errors(),"missing node");
    i=inputs();i.temperature_kelvin.reset();require(align(m,p,i).has_errors(),"missing temperature");
    i=inputs();i.node_voltages["typo"]=1;require(align(m,p,i).has_errors(),"unknown node");
    i=inputs();i.step_limit=1;require(align(m,p,i).has_errors(),"step limit");
    const auto other=sample();
    require(code(M::evaluate_mir(other,p,inputs()).diagnostics,"EVAL_HIR_ERRORS"),"wrong environment identity");
}
void expressions() {
    const auto m=text(R"(module m(a,b); inout a,b; electrical a,b; branch(a,b) ab;
        parameter integer MASK=8'h0F; parameter string S="hello";
        integer x,y; real ids,z; string label;
        analog begin x=5/2; y=MASK&3; ids=1;
        if(0 && sqrt(-1.0)) ids=20;
        if(1 || (1.0/0.0)) ids+=2;
        z=0 ? sqrt(-1.0) : (1 ? 4 : 1.0/0.0); ids=ids+z;
        label=S; I(ab)<+V(a,b); I(b,a)<+1; I(ab)<+2;
        $display("ids=%g",ids); $write("mask=%g",MASK); end endmodule)");
    H::EvalInputs i;i.node_voltages={{"a",3},{"b",1}};
    const auto r=align(m,H::resolve_parameters(*m.hir),i);
    close(H::number(r.observations.at("ids")),7,"nested short circuit");
    close(r.branch_flows.at("ab"),3,"oriented contributions");
    const auto dep=text(R"(module m; parameter real A=2; parameter real B=A*3;
        real ids; analog ids=B; endmodule)");
    (void)align(dep,H::resolve_parameters(*dep.hir,{{"A",4.0}}),{});
    for(const auto& expression:{"1.0/0.0","sqrt(-1.0)","exp(1000.0)","2147483647+1","ids+1"}) {
        const auto bad=text("module m; real ids; analog ids="+std::string(expression)+"; endmodule");
        require(align(bad,H::resolve_parameters(*bad.hir),{}).has_errors(),"runtime diagnostic alignment");
    }
}
}
int main() {
    try {model();expressions();}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"MIR and HIR independent control-flow execution aligned\n";
}
