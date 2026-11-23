#include "vagrad/frontend/hir/eval.hpp"
#include "vagrad/frontend/parser/parser.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace vagrad;
namespace H=vagrad::hir;
void require(bool b,const std::string& text) { if(!b) throw std::runtime_error(text); }
bool code(const H::EvalResult& r,std::string_view c) {
    return std::any_of(r.diagnostics.begin(),r.diagnostics.end(),[=](const auto& d) {return d.code==c;});
}
H::HirModule lower(SourceManager& s,FileId file,const PreprocessOptions& opts={}) {
    const auto parsed=parse(std::make_shared<const PreprocessResult>(preprocess(s,file,opts)));
    require(!parsed.has_errors(),"H3 syntax valid");
    auto m=H::lower_hir(std::make_shared<const H::BindingResult>(H::bind_names(build_syntax_tree(parsed))));
    if(m.has_errors()) for(const auto& d:m.diagnostics) std::cerr<<d.code<<": "<<d.message<<'\n';
    require(!m.has_errors(),"H3 typed HIR valid");
    return m;
}
H::HirModule sample(const PreprocessOptions& opts={}) {
    SourceManager s;
    return lower(s,s.load(std::filesystem::path(VAGRAD_TEST_DATA_DIR)/"simple_mos.va"),opts);
}
H::HirModule text(std::string source) {
    SourceManager s; return lower(s,s.add_virtual("<eval-test.va>",std::move(source)));
}
H::EvalInputs inputs(double vg=1,double vd=.05,double temperature=300) {
    H::EvalInputs i; i.node_voltages={{"g",vg},{"d",vd},{"s",0}}; i.temperature_kelvin=temperature; return i;
}
void close(double a,double b,const std::string& text) {
    require(std::abs(a-b)<=1e-18+1e-12*std::abs(b),text+" numerical mismatch");
}
double observation(const H::EvalResult& r,std::string_view name) {
    require(!r.has_errors(),"result successful");
    return H::number(r.observations.at(std::string(name)));
}
void numeric_table() {
    const auto m=sample();
    const auto params=H::resolve_parameters(m);
    struct Case {double vg,vd,t;int enable;double ids,flow;};
    const std::vector<Case> cases{
        {.3,.05,300,1,5e-14,5e-14},
        {1,.05,300,1,2.625e-5,2.625e-5},
        {1,.8,300,1,1.5125e-4,1.5125e-4},
        {1,.55,300,1,1.5125e-4,1.5125e-4},
        {1,.05,300,0,5e-14,5e-14},
        {1,.05,600,1,2.625e-5,1.3125e-5}
    };
    for(const auto& c:cases) {
        const auto p=c.enable ? params:H::resolve_parameters(m,{{"ENABLE",std::int32_t{0}}});
        const auto r=H::evaluate_hir(m,p,inputs(c.vg,c.vd,c.t));
        close(observation(r,"ids"),c.ids,"hand reference ids");
        close(r.branch_flows.at("ds"),c.flow,"hand reference flow");
    }
    const auto gain=H::evaluate_hir(m,H::resolve_parameters(m,{{"G0",.002}}),inputs());
    close(observation(gain,"ids"),5.25e-5,"gain override");
    const auto threshold=H::evaluate_hir(m,H::resolve_parameters(m,{{"VTH0",.6}}),inputs());
    close(observation(threshold,"ids"),1.875e-5,"threshold override");
    const auto nominal=H::evaluate_hir(m,H::resolve_parameters(m,{{"TNOM",600.0}}),inputs());
    close(observation(nominal,"ids"),2.625e-5,"TNOM does not change ids");
    close(nominal.branch_flows.at("ds"),5.25e-5,"TNOM changes contributed flow");
    const auto unused=H::evaluate_hir(m,H::resolve_parameters(m,{{"L",2e-6}}),inputs());
    close(observation(unused,"ids"),2.625e-5,"unused parameter");
    // 同一 HIR 交替评估路径，保证无上一次调用的可变变量残留。
    const auto off=H::evaluate_hir(m,params,inputs(.3));
    close(observation(off,"ids"),5e-14,"repeat off");
    close(observation(H::evaluate_hir(m,params,inputs()),"ids"),2.625e-5,"repeat on");
}
void configs_and_records() {
    PreprocessOptions opts; opts.defines={{"SOFT_THRESHOLD","1"},{"TRACE_MOS","1"}};
    const auto m=sample(opts);
    const auto r=H::evaluate_hir(m,H::resolve_parameters(m),inputs());
    const auto vov=.5*(.55+std::sqrt(.55*.55+1e-12));
    close(observation(r,"ids"),.001*(vov*.05-.5*.05*.05),"soft threshold formula");
    require(r.records.size()==1 && r.records[0].function=="$strobe" &&
        r.records[0].arguments.size()==4,"trace arguments captured");
    require(std::get<std::string>(r.records[0].arguments[0]).find('\n')!=std::string::npos,"string escape decoded");
    require(std::get<std::int32_t>(r.records[0].arguments[3])==15,"trace mask decoded");
    require(r.records[0].origin.token_index<m.bindings->syntax.input().tokens.size(),"trace origin");
}
void control_and_types() {
    const auto m=text(R"(module m; integer a,b,c; real ids,x; parameter string LABEL="hello";
        string label; analog begin
        a=5/2; b=-5/2; c=1.5; ids=1;
        if(0 && (1.0/0.0)) ids=9;
        if(1 || sqrt(-1.0)) ids+=2;
        x=1 ? 4 : sqrt(-1.0);
        begin:inner real ids; ids=100; end
        ids=ids+x; label=LABEL; end endmodule)");
    const auto r=H::evaluate_hir(m,H::resolve_parameters(m),{});
    close(observation(r,"a"),2,"integer division");
    close(observation(r,"b"),-2,"integer negative division");
    close(observation(r,"c"),2,"real integer conversion");
    close(observation(r,"ids"),7,"if, compound assign, scope, select");
    require(std::get<std::string>(r.observations.at("label"))=="hello","string ref and assign");
    const auto bit=text(R"(module m; parameter integer MASK=8'h0F; integer a,b,c,d,e; analog begin
        a=MASK & 3; b=1<<3; c=(-4)>>>1; d=(-4)>>1; e=~0; end endmodule)");
    const auto br=H::evaluate_hir(bit,H::resolve_parameters(bit),{});
    close(observation(br,"a"),3,"bitand"); close(observation(br,"b"),8,"shiftleft");
    close(observation(br,"c"),-2,"arithmetic right"); close(observation(br,"d"),2147483646,"logical right");
    close(observation(br,"e"),-1,"bitnot");
    const auto math=text(R"(module m; real ids,vt; analog begin ids=ln(exp(2.0))+log(100.0);
        vt=$vt(300); end endmodule)");
    const auto mr=H::evaluate_hir(math,H::resolve_parameters(math),{});
    close(observation(mr,"ids"),4,"ln/log builtin distinction");
    close(observation(mr,"vt"),.025851999786435,"thermal voltage");
    const auto ground=text(R"(module m(a); inout a; electrical a; branch(a) ag; real ids;
        analog begin ids=V(a, 0); I(ag)<+ids; end endmodule)");
    H::EvalInputs gi;gi.node_voltages={{"a",2}};
    const auto gr=H::evaluate_hir(ground,H::resolve_parameters(ground),gi);
    close(observation(gr,"ids"),2,"one terminal ground"); close(gr.branch_flows.at("ag"),2,"ground flow");
}
void errors_and_guards() {
    const auto m=sample();const auto p=H::resolve_parameters(m);
    auto i=inputs();i.node_voltages.erase("g");
    require(code(H::evaluate_hir(m,p,i),"EVAL_INPUT"),"missing voltage");
    i=inputs();i.temperature_kelvin.reset();
    require(code(H::evaluate_hir(m,p,i),"EVAL_INPUT"),"missing temperature");
    i=inputs();i.temperature_kelvin=0;
    require(code(H::evaluate_hir(m,p,i),"EVAL_INPUT"),"invalid temperature");
    i=inputs();i.node_voltages["gg"]=1;
    require(code(H::evaluate_hir(m,p,i),"EVAL_INPUT"),"typo node");
    i=inputs();i.step_limit=1;
    require(code(H::evaluate_hir(m,p,i),"EVAL_LIMIT"),"execution budget");
    const auto other=sample(); // 同样个数的 Symbol 也不能共用参数环境
    require(code(H::evaluate_hir(other,p,inputs()),"EVAL_HIR_ERRORS"),"environment identity");
    require(code(H::evaluate_hir(m,H::resolve_parameters(m,{{"G0",-1.0}}),inputs()),"EVAL_HIR_ERRORS"),"invalid environment rejected");
    for(const auto& [expression,expected]:std::vector<std::pair<std::string,std::string>>{
        {"1.0/0.0","EVAL_DIV_ZERO"},{"sqrt(-1.0)","EVAL_DOMAIN"},{"exp(1000.0)","EVAL_DOMAIN"},
        {"2147483647+1","EVAL_INTEGER_RANGE"},{"1<<32","EVAL_INTEGER_RANGE"}}) {
        const auto bad=text("module m; real ids; analog ids="+expression+"; endmodule");
        const auto r=H::evaluate_hir(bad,H::resolve_parameters(bad),{});
        require(code(r,expected) && r.observations.empty(),"runtime failure is not a valid observation");
        require(r.diagnostics.front().primary.has_value(),"runtime source position");
    }
    const auto state=text(R"(module m; real ids; analog ids=ids+1; endmodule)");
    require(code(H::evaluate_hir(state,H::resolve_parameters(state),{}),"EVAL_STATE"),"read before assign cannot assume zero");
}
}
int main() {
    try {numeric_table();configs_and_records();control_and_types();errors_and_guards();}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"H3 independent numerical table, control, inputs and error states passed\n";
}
