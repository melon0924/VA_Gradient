// 面向 simple_mos.va 的教学入口：解析一次，然后运行多组偏置。
// 这不是通用 CSV 命令；参数名 G0/VTH0/TNOM 和节点 d/g/s 来自该示例模型。
#include "vagrad/ad/eval.hpp"
#include "vagrad/frontend/parser/parser.hpp"
#include <filesystem>
#include <iostream>

namespace {
int run(const std::filesystem::path& file) {
    namespace V=vagrad;namespace H=V::hir;namespace M=V::mir;namespace A=V::ad;
    V::SourceManager sources;
    auto errors=[&](const std::vector<V::Diagnostic>& diagnostics) {
        bool failed=false;
        for(const auto& d:diagnostics) {
            V::print_diagnostic(std::cerr,sources,d);
            failed=failed||d.severity==V::Severity::Error;
        }
        return failed;
    };
    // 1. 统一前端链。include、宏、Parser、CST 与 HIR 都使用正式库。
    auto expanded=V::preprocess(sources,sources.load(file));
    if(errors(expanded.diagnostics)) return 1;
    const auto parsed=V::parse(std::make_shared<const V::PreprocessResult>(std::move(expanded)));
    if(errors(parsed.diagnostics)) return 1;
    const auto syntax=V::build_syntax_tree(parsed);
    auto names=H::bind_names(syntax);
    if(errors(names.diagnostics)) return 1;
    const auto hir=std::make_shared<const H::HirModule>(
        H::lower_hir(std::make_shared<const H::BindingResult>(std::move(names))));
    if(errors(hir->diagnostics)) return 1;

    // 2. HIR -> MIR-Local -> SSA。保留参数 Ref；不按默认值折叠成常数。
    const auto ssa=M::promote_to_ssa(M::lower_mir(hir));
    const auto parameters=H::resolve_parameters(*hir);
    if(errors(parameters.diagnostics)) return 1;

    // 3. 准备只读执行快照：校验、导数 IR、种子支持集只在此建立。
    const auto compiled=A::prepare(ssa,parameters,{"G0","VTH0","TNOM"});
    if(errors(compiled.diagnostics())) return 1;

    // 4. 每组输入独立。既可以复用参数，也可以 resolve_parameters 提供新参数卡。
    std::vector<A::BatchCase> cases;
    for(const double gate:{.3,.7,1.,1.2}) {
        H::EvalInputs bias;
        bias.node_voltages={{"d",.05},{"g",gate},{"s",0}};
        bias.temperature_kelvin=300;
        cases.push_back({parameters,std::move(bias)});
    }
    const auto results=A::evaluate_batch(compiled,cases);

    // 5. 输出列顺序由种子列表确定。失败行不会伪装成零导数。
    std::cout<<"Vg,ids,d_ids_d_G0,d_ids_d_VTH0,d_flow_ds_d_TNOM\n";
    for(std::size_t row=0;row<results.size();++row) {
        const auto& r=results[row];
        if(r.has_errors()) {errors(r.diagnostics);errors(r.primal.diagnostics);return 1;}
        std::cout<<H::scalar_text(cases[row].inputs.node_voltages.at("g"))<<','
                 <<H::scalar_text(r.primal.observations.at("ids"))<<','
                 <<H::scalar_text(r.observations.at("ids")[0])<<','
                 <<H::scalar_text(r.observations.at("ids")[1])<<','
                 <<H::scalar_text(r.branch_flows.at("ds")[2])<<'\n';
    }
    return 0;
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t* argv[]) {
#else
int main(int argc,char* argv[]) {
#endif
    if(argc!=2) {std::cerr<<"Usage: vagrad_ad_demo <simple_mos.va>\n";return 2;}
    try {return run(std::filesystem::path(argv[1]));}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
