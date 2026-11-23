#pragma once
#include "vagrad/ad/state.hpp"
#include "vagrad/ad/prepared.hpp"
namespace vagrad::ad {
enum class Storage { Sparse, Dense };
struct Result {
    hir::EvalResult primal;
    std::vector<std::string> seeds;
    std::map<std::string,std::vector<double>> observations,branch_flows;
    std::vector<Diagnostic> diagnostics;
    ParameterMode mode=ParameterMode::Effective;
    Storage storage=Storage::Sparse;
    std::size_t tangent_slots=0,dense_tangent_slots=0;
    bool has_errors() const {return primal.has_errors()||!diagnostics.empty();}
};
Result evaluate(const mir::MirFunction&,const hir::ParamEnv&,const hir::EvalInputs&,const std::vector<std::string>& seeds,
                ParameterMode mode=ParameterMode::Effective,Storage storage=Storage::Sparse);
// 复用校验后的 MIR、导数 IR 和支持集；调用方仍通过 resolve_parameters 提供有效环境。
Result evaluate(const Prepared&,const hir::ParamEnv&,const hir::EvalInputs&,Storage storage=Storage::Sparse);
struct BatchCase {hir::ParamEnv parameters;hir::EvalInputs inputs;};
std::vector<Result> evaluate_batch(const Prepared&,const std::vector<BatchCase>&,Storage storage=Storage::Sparse);
void print_result(std::ostream&,const Result&);
} // namespace vagrad::ad
