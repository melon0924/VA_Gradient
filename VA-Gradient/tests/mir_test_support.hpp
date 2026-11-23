#pragma once
#include "vagrad/ir/mir/eval.hpp"
#include "vagrad/frontend/parser/parser.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace mir_test {
using namespace vagrad;
namespace H=vagrad::hir;
namespace M=vagrad::mir;
inline void require(bool ok,const std::string& text) {if(!ok) throw std::runtime_error(text);}
inline bool code(const std::vector<Diagnostic>& ds,std::string_view c) {
    return std::any_of(ds.begin(),ds.end(),[=](const auto& d){return d.code==c;});
}
inline std::shared_ptr<const H::HirModule> lower(SourceManager& s,FileId id,const PreprocessOptions& options={}) {
    const auto p=parse(std::make_shared<const PreprocessResult>(preprocess(s,id,options)));
    require(!p.has_errors(),"fixture syntax valid");
    auto h=std::make_shared<const H::HirModule>(H::lower_hir(
        std::make_shared<const H::BindingResult>(H::bind_names(build_syntax_tree(p)))));
    require(!h->has_errors(),"fixture HIR valid");
    return h;
}
inline M::MirFunction text(std::string source) {
    SourceManager s;return M::lower_mir(lower(s,s.add_virtual("<mir-test.va>",std::move(source))));
}
inline M::MirFunction sample(const PreprocessOptions& options={}) {
    SourceManager s;
    return M::lower_mir(lower(s,s.load(std::filesystem::path(VAGRAD_TEST_DATA_DIR)/"simple_mos.va"),options));
}
inline void close(double a,double b,const std::string& text) {
    require(std::abs(a-b)<=1e-18+1e-12*std::abs(b),text+" numerical mismatch");
}
inline H::EvalInputs inputs(double vg=1,double vd=.05,double t=300) {
    H::EvalInputs i;i.node_voltages={{"g",vg},{"d",vd},{"s",0}};i.temperature_kelvin=t;return i;
}
}
