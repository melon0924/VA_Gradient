#include "vagrad/common/diagnostic.hpp"
#include "vagrad/common/source_manager.hpp"
#include "vagrad/frontend/lexer/lexer.hpp"
#include "vagrad/frontend/preprocessor/preprocessor.hpp"
#include "vagrad/frontend/parser/parser.hpp"
#include "vagrad/frontend/syntax/syntax_tree.hpp"
#include "vagrad/frontend/ast/ast.hpp"
#include "vagrad/frontend/hir/hir.hpp"
#include "vagrad/frontend/hir/eval.hpp"
#include "vagrad/ir/mir/eval.hpp"
#include "vagrad/analysis/dominance.hpp"
#include "vagrad/ir/mir/slice.hpp"
#include "vagrad/ad/eval.hpp"
#include "vagrad/ad/ir.hpp"
#include "vagrad/validation/gradcheck.hpp"
#include "vagrad/codegen/c.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
enum class FrontendStage { Preprocess, Parse, Ast, Hir, Eval, Mir, Cfg, Slice, AdPlan, GradCheck, EmitC };

void usage(std::ostream& out) {
    out << "Usage:\n"
        << "  vagrad source <file.va>\n"
        << "  vagrad locate <file.va> <byte-offset>\n"
        << "  vagrad lex <file.va> [-o <tokens.txt>]\n"
        << "  vagrad preprocess <file.va> [-D NAME[=VALUE]] [-I directory]\n"
        << "                    [--dump-tokens] [-o <output>]\n"
        << "  vagrad parse <file.va> [-D NAME[=VALUE]] [-I directory]\n"
        << "               [--dump-events] [-o <output>]\n"
        << "  vagrad ast <file.va> [-D NAME[=VALUE]] [-I directory] [-o <output>]\n"
        << "  vagrad hir <file.va> [--typed [--param NAME=NUMBER]] [-D NAME[=VALUE]]\n"
        << "             [-I directory] [-o <output>]\n"
        << "  vagrad eval <file.va> [--param NAME=NUMBER] [--voltage NODE=NUMBER]\n"
        << "              [--temperature KELVIN] [--ir hir|mir-local|mir-ssa|mir-slice]\n"
        << "              [--target VARIABLE@exit|flow(BRANCH)@exit] [--effects preserve|target-only]\n"
        << "              [--wrt REAL_PARAMETER] (repeat for multiple derivative seeds)\n"
        << "              [--parameter-derivatives effective|defaults] (requires --wrt)\n"
        << "              [--ad-storage sparse|dense] (requires --wrt)\n"
        << "              [-D NAME[=VALUE]] [-I directory] [-o <output>]\n"
        << "  vagrad mir <file.va> [--form local|ssa] [-D NAME[=VALUE]] [-I directory] [-o <output>]\n"
        << "  vagrad cfg <file.va> [--dominators] [-D NAME[=VALUE]] [-I directory] [-o <output>]\n"
        << "  vagrad slice <file.va> --target VARIABLE@exit|flow(BRANCH)@exit\n"
        << "               [--effects preserve|target-only] [-D NAME[=VALUE]] [-I directory] [-o <output>]\n";
    out << "  vagrad ad-plan <file.va> --wrt REAL_PARAMETER [--wrt ...]\n"
        << "                 [--parameter-derivatives effective|defaults] [--param NAME=NUMBER]\n"
        << "                 [--ir mir-ssa|mir-slice --target ... --effects ...]\n"
        << "                 [-D NAME[=VALUE]] [-I directory] [-o <output>]\n";
    out << "  vagrad gradcheck <file.va> --wrt REAL_PARAMETER [--wrt ...]\n"
        << "                   [eval inputs/options] [--step PARAMETER=POSITIVE_NUMBER]\n"
        << "                   [--rtol NUMBER] [--atol NUMBER] [-o <report>]\n";
    out << "  vagrad emit-c <file.va> --wrt REAL_PARAMETER [--wrt ...] [--param NAME=NUMBER]\n"
        << "                [--ir mir-ssa|mir-slice --target ... --effects ...] [-o <model.c>]\n";
}

// 所有文本输出都检查各自加载的输入，避免 -o 覆盖主文件或头文件。
bool open_output(std::ofstream& output, const fs::path& path,
                 const std::vector<fs::path>& input_paths) {
    for (const auto& input : input_paths) {
        std::error_code error;
        if (fs::equivalent(input, path, error)) {
            std::cerr << "error[CLI_OUTPUT]: output path refers to an input file\n";
            return false;
        }
    }
    output.open(path, std::ios::binary);
    if (!output) {
        std::cerr << "error[CLI_OUTPUT]: could not open " << vagrad::path_utf8(path) << '\n';
        return false;
    }
    return true;
}

int run_lex(vagrad::SourceManager& sources,
            std::shared_ptr<const vagrad::SourceFile> file,
            const std::vector<fs::path>& args) {
    // lex 只扫描当前文件；include 与宏统一交给 preprocess。
    const auto result = vagrad::lex(file);
    std::ofstream output;
    if (args.size() == 4) {
        if (!open_output(output, args[3], {file->path()})) {
            return 1;
        }
    }
    auto& out = output.is_open() ? static_cast<std::ostream&>(output) : std::cout;
    vagrad::print_tokens(out, result);
    out.flush();
    if (!out) {
        std::cerr << "error[CLI_OUTPUT]: failed while writing tokens\n";
        return 1;
    }
    for (const auto& diagnostic : result.diagnostics) {
        vagrad::print_diagnostic(std::cerr, sources, diagnostic);
    }
    return result.has_errors() ? 1 : 0;
}

int run_frontend(vagrad::SourceManager& sources, vagrad::FileId file,
                 const std::vector<fs::path>& args, FrontendStage stage) {
    vagrad::PreprocessOptions options;
    std::optional<fs::path> output_path;
    bool dump = false;
    bool dominators=false;
    const bool mir_stage=stage==FrontendStage::Mir||stage==FrontendStage::Cfg||stage==FrontendStage::Slice;
    const bool ad_plan_stage=stage==FrontendStage::AdPlan;
    const bool gradient_check_stage=stage==FrontendStage::GradCheck;
    const bool emit_c_stage=stage==FrontendStage::EmitC;
    const bool execute_stage=stage==FrontendStage::Eval||gradient_check_stage;
    const bool evaluation_options=execute_stage||ad_plan_stage||emit_c_stage;
    bool typed = evaluation_options || mir_stage;
    std::optional<std::string> eval_ir,mir_form;
    std::optional<std::string> slice_target,slice_effects;
    std::vector<std::string> ad_seeds;
    std::optional<vagrad::ad::ParameterMode> ad_mode;
    std::optional<vagrad::ad::Storage> ad_storage;
    std::map<std::string, vagrad::hir::Scalar> overrides;
    vagrad::hir::EvalInputs eval_inputs;
    vagrad::validation::GradientOptions check_options;
    bool relative_tolerance_set=false,absolute_tolerance_set=false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const auto flag = vagrad::path_utf8(args[i]);
        if ((flag == "--dump-tokens" && stage == FrontendStage::Preprocess) ||
            (flag == "--dump-events" && stage == FrontendStage::Parse)) {
            dump = true;
        } else if (flag == "--typed" && stage == FrontendStage::Hir && !typed) {
            typed = true;
        } else if(flag=="--dominators"&&stage==FrontendStage::Cfg&&!dominators) {
            dominators=true;
        } else if(flag=="--wrt"&&evaluation_options&&i+1<args.size()) {
            ad_seeds.push_back(vagrad::path_utf8(args[++i]));
        } else if(flag=="--parameter-derivatives"&&evaluation_options&&!ad_mode) {
            if(i+1==args.size()) {std::cerr<<"error[CLI_OPTION]: missing parameter derivative mode\n";return 2;}
            const auto value=vagrad::path_utf8(args[++i]);
            if(value=="effective") ad_mode=vagrad::ad::ParameterMode::Effective;
            else if(value=="defaults") ad_mode=vagrad::ad::ParameterMode::Defaults;
            else {std::cerr<<"error[CLI_OPTION]: unsupported parameter derivative mode\n";return 2;}
        } else if(flag=="--ad-storage"&&stage==FrontendStage::Eval&&!ad_storage) {
            if(i+1==args.size()) {std::cerr<<"error[CLI_OPTION]: missing AD storage mode\n";return 2;}
            const auto value=vagrad::path_utf8(args[++i]);
            if(value=="sparse") ad_storage=vagrad::ad::Storage::Sparse;
            else if(value=="dense") ad_storage=vagrad::ad::Storage::Dense;
            else {std::cerr<<"error[CLI_OPTION]: unsupported AD storage mode\n";return 2;}
        } else if((stage==FrontendStage::Slice||evaluation_options)&&
                  ((flag=="--target"&&!slice_target)||(flag=="--effects"&&!slice_effects))) {
            if(i+1==args.size()) {std::cerr<<"error[CLI_OPTION]: missing operand for "<<flag<<'\n';return 2;}
            const auto value=vagrad::path_utf8(args[++i]);
            if(flag=="--target") slice_target=value;
            else if(value=="preserve"||value=="target-only") slice_effects=value;
            else {std::cerr<<"error[CLI_OPTION]: unsupported effect policy\n";return 2;}
        } else if ((flag=="--ir" && evaluation_options && !eval_ir) ||
                   (flag=="--form" && stage==FrontendStage::Mir && !mir_form)) {
            if(i+1==args.size()) {std::cerr<<"error[CLI_OPTION]: missing operand for "<<flag<<'\n';return 2;}
            const auto operand=vagrad::path_utf8(args[++i]);
            if(flag=="--ir" && (operand=="hir"||operand=="mir-local"||operand=="mir-ssa"||operand=="mir-slice")) eval_ir=operand;
            else if(flag=="--form" && (operand=="local"||operand=="ssa")) mir_form=operand;
            else {std::cerr<<"error[CLI_OPTION]: unsupported "<<flag<<" value: "<<operand<<'\n';return 2;}
        } else if ((stage == FrontendStage::Hir || evaluation_options) &&
                   (flag == "--param" || (execute_stage &&
                    (flag == "--voltage" || flag == "--temperature")) || (gradient_check_stage&&
                    (flag=="--step"||flag=="--rtol"||flag=="--atol"))) && i + 1 < args.size()) {
            const auto operand = vagrad::path_utf8(args[++i]);
            const auto equal = operand.find('=');
            const bool temperature = flag == "--temperature";
            const bool tolerance=flag=="--rtol"||flag=="--atol";
            if (!temperature && !tolerance && (equal == operand.npos || equal == 0 || equal + 1 == operand.size())) {
                std::cerr << "error[CLI_OPTION]: expected NAME=NUMBER for " << flag << '\n'; return 2;
            }
            const auto name = temperature||tolerance ? std::string{} : operand.substr(0, equal);
            auto text = temperature||tolerance ? operand : operand.substr(equal + 1);
            if (text.starts_with('+')) text.erase(0, 1);
            vagrad::hir::LiteralFormat format; std::string error;
            const auto value = vagrad::hir::decode_number(text, format, error);
            if (!value) { std::cerr << "error[CLI_NUMBER]: " << error << '\n'; return 2; }
            bool unique = true;
            if (flag == "--param") unique = overrides.emplace(name, *value).second;
            else if (flag == "--voltage") unique = eval_inputs.node_voltages.emplace(name, vagrad::hir::number(*value)).second;
            else if(flag=="--step") unique=check_options.steps.emplace(name,vagrad::hir::number(*value)).second;
            else if(tolerance) {
                auto& seen=flag=="--rtol"?relative_tolerance_set:absolute_tolerance_set;
                unique=!seen;seen=true;
                (flag=="--rtol"?check_options.relative_tolerance:check_options.absolute_tolerance)=vagrad::hir::number(*value);
            }
            else {
                unique = !eval_inputs.temperature_kelvin;
                eval_inputs.temperature_kelvin = vagrad::hir::number(*value);
            }
            if (!unique) { std::cerr << "error[CLI_OPTION]: duplicate input " << flag << ' ' << name << '\n'; return 2; }
        } else if (flag == "-o" && i + 1 < args.size() && !output_path) {
            output_path = args[++i];
        } else if (flag.starts_with("-D") || flag.starts_with("-I")) {
            const bool define = flag[1] == 'D';
            fs::path operand;
            if (flag.size() == 2) {
                if (i + 1 == args.size()) {
                    std::cerr << "error[CLI_OPTION]: missing operand for " << flag << '\n';
                    return 2;
                }
                operand = args[++i];
            } else {
                const auto bytes = flag.substr(2);
                operand = fs::path(std::u8string(bytes.begin(), bytes.end()));
            }
            if (define) {
                const auto value = vagrad::path_utf8(operand);
                const auto equal = value.find('=');
                options.defines[value.substr(0, equal)] =
                    equal == value.npos ? "1" : value.substr(equal + 1);
            } else {
                options.include_dirs.push_back(operand);
            }
        } else {
            std::cerr << "error[CLI_OPTION]: invalid or incomplete option: " << flag << '\n';
            return 2;
        }
    }
    if (!typed && !overrides.empty()) {
        std::cerr << "error[CLI_OPTION]: --param on hir requires --typed\n"; return 2;
    }
    if((ad_mode||ad_storage||ad_plan_stage||gradient_check_stage||emit_c_stage)&&ad_seeds.empty()) {
        std::cerr<<"error[CLI_OPTION]: AD options/ad-plan require --wrt\n";return 2;
    }
    if(!ad_seeds.empty()) {
        if(!eval_ir) eval_ir="mir-ssa";
        if(*eval_ir!="mir-ssa"&&*eval_ir!="mir-slice") {
            std::cerr<<"error[CLI_OPTION]: --wrt requires mir-ssa or mir-slice\n";return 2;
        }
    }
    const bool slicing=stage==FrontendStage::Slice||(eval_ir&&*eval_ir=="mir-slice");
    if((slicing&&!slice_target)||(!slicing&&(slice_target||slice_effects))) {
        std::cerr<<"error[CLI_OPTION]: --target is required for slice/mir-slice; slice options require that mode\n";return 2;
    }
    auto result = vagrad::preprocess(sources, file, options);
    for (const auto& diagnostic : result.diagnostics) {
        vagrad::print_diagnostic(std::cerr, sources, diagnostic);
    }
    if (stage != FrontendStage::Preprocess && result.has_errors()) {
        return 1; // 先修复预处理错误，避免把部分展开结果当作完整语法输入。
    }
    std::vector<fs::path> input_paths;
    for (const auto& [id, input] : result.files) {
        (void)id;
        input_paths.push_back(input->path());
    }
    bool failed = result.has_errors();
    std::optional<vagrad::ParseResult> parsed;
    std::optional<vagrad::SyntaxTree> syntax;
    std::optional<vagrad::hir::BindingResult> bindings;
    std::shared_ptr<const vagrad::hir::HirModule> hir;
    std::optional<vagrad::hir::ParamEnv> defaults, effective;
    std::optional<vagrad::hir::EvalResult> evaluation;
    std::optional<vagrad::ad::Result> differentiated;
    std::optional<vagrad::ad::Plan> derivative_plan;
    std::optional<vagrad::ad::Sparsity> sparsity;
    std::optional<vagrad::ad::DerivativeIr> derivative_ir;
    std::optional<vagrad::validation::GradientCheck> checked;
    std::optional<vagrad::codegen::CSource> generated_c;
    std::optional<vagrad::mir::MirFunction> mir;
    std::optional<vagrad::mir::CfgInfo> cfg;
    const bool eval_mir=eval_ir && *eval_ir!="hir";
    const bool ssa=slicing||(mir_form&&*mir_form=="ssa")||(eval_ir&&*eval_ir=="mir-ssa");
    std::optional<vagrad::mir::DominanceInfo> dominance;
    std::optional<vagrad::mir::SliceResult> slice;
    const bool semantic = stage == FrontendStage::Hir || evaluation_options || mir_stage;
    if (stage != FrontendStage::Preprocess) {
        // 所有后续命令复用同一条解析/构树链，H1 消费现有 AST View。
        parsed.emplace(vagrad::parse(
            std::make_shared<const vagrad::PreprocessResult>(std::move(result))));
        for (const auto& diagnostic : parsed->diagnostics) {
            vagrad::print_diagnostic(std::cerr, sources, diagnostic);
        }
        failed = parsed->has_errors();
        if (semantic && failed) return 1;
        if (!(stage == FrontendStage::Parse && dump)) {
            syntax.emplace(vagrad::build_syntax_tree(*parsed));
        }
        if (semantic) {
            bindings.emplace(vagrad::hir::bind_names(*syntax));
            for (const auto& diagnostic : bindings->diagnostics) {
                vagrad::print_diagnostic(std::cerr, sources, diagnostic);
            }
            failed = bindings->has_errors();
            // H1 的默认报告保持原格式；H2/H3 严格复用同一次绑定结果。
            if (typed) {
                if (failed) return 1;
                hir=std::make_shared<const vagrad::hir::HirModule>(vagrad::hir::lower_hir(
                    std::make_shared<const vagrad::hir::BindingResult>(std::move(*bindings))));
                for (const auto& d : hir->diagnostics) vagrad::print_diagnostic(std::cerr, sources, d);
                failed = hir->has_errors();
                if(!failed && (mir_stage||eval_mir)) {
                    mir.emplace(vagrad::mir::lower_mir(hir));
                    if(ssa) *mir=vagrad::mir::promote_to_ssa(*mir);
                    if(slicing) {
                        const auto policy=slice_effects&&*slice_effects=="target-only"?
                            vagrad::mir::SliceEffects::TargetOnly:vagrad::mir::SliceEffects::Preserve;
                        slice.emplace(vagrad::mir::slice_ssa(*mir,*slice_target,policy));
                        for(const auto& d:slice->diagnostics) vagrad::print_diagnostic(std::cerr,sources,d);
                        if(slice->has_errors()) return 1;
                        *mir=slice->function;
                    }
                    const auto diagnostics=vagrad::mir::verify_mir(*mir);
                    for(const auto& d:diagnostics) vagrad::print_diagnostic(std::cerr,sources,d);
                    failed=mir->has_errors()||!diagnostics.empty();
                    if(!failed && stage==FrontendStage::Cfg) {
                        cfg.emplace(vagrad::mir::build_cfg(*mir));
                        failed=cfg->has_errors();
                        if(!failed&&dominators) {
                            dominance.emplace(vagrad::mir::analyze_dominance(*mir));
                            failed=dominance->has_errors();
                            for(const auto& d:dominance->diagnostics) vagrad::print_diagnostic(std::cerr,sources,d);
                        }
                    }
                }
                if (!failed && !mir_stage) {
                    defaults.emplace(vagrad::hir::resolve_parameters(*hir));
                    effective.emplace(overrides.empty() ? *defaults : vagrad::hir::resolve_parameters(*hir, overrides));
                    for (const auto& d : effective->diagnostics) vagrad::print_diagnostic(std::cerr, sources, d);
                    failed = effective->has_errors();
                    if(!failed&&emit_c_stage) {
                        generated_c.emplace(vagrad::codegen::emit_c(vagrad::ad::prepare(*mir,*effective,ad_seeds,
                            ad_mode.value_or(vagrad::ad::ParameterMode::Effective)),*effective));
                        for(const auto& d:generated_c->diagnostics) vagrad::print_diagnostic(std::cerr,sources,d);
                        failed=generated_c->has_errors();
                    }
                    if(!failed&&ad_plan_stage) {
                        derivative_plan.emplace(vagrad::ad::make_plan(*mir,ad_seeds,
                            ad_mode.value_or(vagrad::ad::ParameterMode::Effective)));
                        for(const auto& d:derivative_plan->diagnostics) vagrad::print_diagnostic(std::cerr,sources,d);
                        failed=derivative_plan->has_errors();
                        if(!failed) {
                            sparsity.emplace(vagrad::ad::analyze_sparsity(*mir,*derivative_plan,*effective));
                            for(const auto& d:sparsity->diagnostics) vagrad::print_diagnostic(std::cerr,sources,d);
                            failed=sparsity->has_errors();
                            if(!failed) {
                                derivative_ir.emplace(vagrad::ad::lower_derivatives(*mir));
                                for(const auto& d:derivative_ir->diagnostics) vagrad::print_diagnostic(std::cerr,sources,d);
                                failed=derivative_ir->has_errors();
                            }
                        }
                    }
                    // 默认环境可能有循环，但合法覆盖可打断默认依赖；以有效环境决定执行。
                    if (!failed && execute_stage) {
                        if(gradient_check_stage) {
                            checked.emplace(vagrad::validation::check_gradients(*mir,*effective,eval_inputs,ad_seeds,check_options,
                                ad_mode.value_or(vagrad::ad::ParameterMode::Effective)));
                            for(const auto& d:checked->diagnostics) vagrad::print_diagnostic(std::cerr,sources,d);
                            for(const auto& d:checked->automatic.diagnostics) vagrad::print_diagnostic(std::cerr,sources,d);
                            for(const auto& d:checked->automatic.primal.diagnostics) vagrad::print_diagnostic(std::cerr,sources,d);
                            failed=checked->has_errors();
                        } else if(!ad_seeds.empty()) {
                            differentiated.emplace(vagrad::ad::evaluate(*mir,*effective,eval_inputs,ad_seeds,
                                ad_mode.value_or(vagrad::ad::ParameterMode::Effective),
                                ad_storage.value_or(vagrad::ad::Storage::Sparse)));
                            for(const auto& d:differentiated->diagnostics) vagrad::print_diagnostic(std::cerr,sources,d);
                            for(const auto& d:differentiated->primal.diagnostics) vagrad::print_diagnostic(std::cerr,sources,d);
                            failed=differentiated->has_errors();
                        } else {
                            evaluation.emplace(eval_mir?vagrad::mir::evaluate_mir(*mir,*effective,eval_inputs):
                                vagrad::hir::evaluate_hir(*hir, *effective, eval_inputs));
                            for (const auto& d : evaluation->diagnostics) vagrad::print_diagnostic(std::cerr, sources, d);
                            failed = evaluation->has_errors();
                        }
                    }
                }
                // eval 失败时不替换上一次成功的数值结果。
                if ((evaluation_options || mir_stage) && failed) return 1;
            }
        }
    }
    std::ofstream output;
    if (output_path && !open_output(output, *output_path, input_paths)) return 1;
    auto& out = output.is_open() ? static_cast<std::ostream&>(output) : std::cout;
    if(emit_c_stage) {
        out<<generated_c->text;
    } else if(gradient_check_stage) {
        vagrad::validation::print_gradient_check(out,*checked);
        failed=!checked->passed(); // 完成的数值核验即使不匹配，也输出 PASS/FAIL 报告。
    } else if(ad_plan_stage) {
        vagrad::ad::print_sparsity(out,*sparsity,*derivative_plan);
        vagrad::mir::print_mir(out,*mir);
        vagrad::ad::print_derivatives(out,*mir,*derivative_ir);
    } else if(stage==FrontendStage::Slice) {
        vagrad::mir::print_slice(out,*slice);
    } else if(stage==FrontendStage::Mir) {
        vagrad::mir::print_mir(out,*mir);
    } else if(stage==FrontendStage::Cfg) {
        vagrad::mir::print_cfg(out,*mir,*cfg);
        if(dominance) vagrad::mir::print_dominance(out,*dominance);
    } else if (stage == FrontendStage::Eval) {
        vagrad::hir::print_parameters(out, *hir, *defaults, *effective);
        if(differentiated) vagrad::ad::print_result(out,*differentiated);
        else vagrad::hir::print_evaluation(out, *evaluation,slicing?"SSA-Slice":ssa?"MIR-SSA":eval_mir?"MIR-Local":"HIR H3");
    } else if (stage == FrontendStage::Hir && typed) {
        vagrad::hir::print_typed_hir(out, *hir);
        if (defaults && effective) vagrad::hir::print_parameters(out, *hir, *defaults, *effective);
        if (effective) for (const auto& d : effective->diagnostics)
            out << "Diagnostic " << d.code << ": " << d.message << '\n';
    } else if (stage == FrontendStage::Hir) {
        vagrad::hir::print_bindings(out, *bindings);
    } else if (stage == FrontendStage::Ast) {
        vagrad::ast::print_ast_view(out, *syntax);
    } else if (stage == FrontendStage::Parse) {
        if (dump) vagrad::print_parse_events(out, *parsed);
        else vagrad::print_syntax_tree(out, *syntax);
    } else if (dump) {
        vagrad::print_preprocessed_tokens(out, result);
    } else {
        vagrad::print_preprocessed_source(out, result);
    }
    out.flush();
    if (!out) {
        std::cerr << "error[CLI_OUTPUT]: failed while writing frontend output\n";
        return 1;
    }
    return failed ? 1 : 0;
}

bool parse_offset(std::string_view text, std::size_t& offset) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), offset);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

int run(const std::vector<fs::path>& args) {
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        usage(std::cout);
        return 0;
    }
    const bool source_command = args.size() == 2 && args[0] == "source";
    const bool locate_command = args.size() == 3 && args[0] == "locate";
    const bool lex_command = (args.size() == 2 || (args.size() == 4 && args[2] == "-o")) &&
                             args[0] == "lex";
    const bool preprocess_command = args.size() >= 2 && args[0] == "preprocess";
    const bool parse_command = args.size() >= 2 && args[0] == "parse";
    const bool ast_command = args.size() >= 2 && args[0] == "ast";
    const bool hir_command = args.size() >= 2 && args[0] == "hir";
    const bool eval_command = args.size() >= 2 && args[0] == "eval";
    const bool mir_command = args.size() >= 2 && args[0] == "mir";
    const bool cfg_command = args.size() >= 2 && args[0] == "cfg";
    const bool slice_command = args.size() >= 2 && args[0] == "slice";
    const bool ad_plan_command = args.size() >= 2 && args[0] == "ad-plan";
    const bool gradcheck_command = args.size() >= 2 && args[0] == "gradcheck";
    const bool emit_c_command = args.size() >= 2 && args[0] == "emit-c";
    if (!source_command && !locate_command && !lex_command && !preprocess_command &&
        !parse_command && !ast_command && !hir_command && !eval_command && !mir_command && !cfg_command && !slice_command && !ad_plan_command && !gradcheck_command && !emit_c_command) {
        usage(std::cerr);
        return 2;
    }

    std::size_t offset = 0;
    if (locate_command && !parse_offset(vagrad::path_utf8(args[2]), offset)) {
        std::cerr << "error[CLI_OFFSET]: byte-offset must be a non-negative integer\n";
        return 2;
    }

    vagrad::SourceManager sources;
    try {
        const auto id = sources.load(args[1]);
        const auto file = sources.get(id);
        if (lex_command) {
            return run_lex(sources, file, args);
        }
        if (preprocess_command || parse_command || ast_command || hir_command || eval_command || mir_command || cfg_command || slice_command || ad_plan_command || gradcheck_command || emit_c_command) {
            const auto stage = emit_c_command?FrontendStage::EmitC:gradcheck_command?FrontendStage::GradCheck:ad_plan_command?FrontendStage::AdPlan:slice_command?FrontendStage::Slice:mir_command?FrontendStage::Mir:cfg_command?FrontendStage::Cfg:
                eval_command ? FrontendStage::Eval : hir_command ? FrontendStage::Hir : ast_command ? FrontendStage::Ast :
                parse_command ? FrontendStage::Parse : FrontendStage::Preprocess;
            return run_frontend(sources, id, args, stage);
        }
        if (source_command) {
            std::cout << "File: " << vagrad::path_utf8(file->path())
                      << "\nFileId: " << id.value
                      << "\nBytes: " << file->text().size()
                      << "\nLines: " << file->line_count() << '\n';
            return 0;
        }
        if (offset > file->text().size()) {
            vagrad::print_diagnostic(std::cerr, sources, {
                vagrad::Severity::Error,
                "SOURCE_OFFSET",
                "offset " + std::to_string(offset) + " exceeds " +
                    std::to_string(file->text().size()) + " source bytes",
                vagrad::SourceSpan{id, file->text().size(), file->text().size()},
                {}
            });
            return 1;
        }
        const auto location = file->locate(offset);
        std::cout << vagrad::path_utf8(file->path()) << ':' << location.line << ':'
                  << location.column << '\n';
        return 0;
    } catch (const std::exception& error) {
        vagrad::print_diagnostic(std::cerr, sources, {
            vagrad::Severity::Error, "SOURCE_LOAD",
            vagrad::path_utf8(args[1]) + ": " + error.what(), std::nullopt, {}
        });
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    // Windows 用宽字符参数接收包含中文的文件路径。
    std::vector<fs::path> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return run(args);
}

