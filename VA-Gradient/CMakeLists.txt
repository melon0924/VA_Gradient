cmake_minimum_required(VERSION 3.21)
project(VAGradient VERSION 0.1.0 LANGUAGES C CXX)

# CLI 和测试链接同一个库，后续前端模块逐步加到此目标中。
add_library(vagrad_frontend
    src/common/source_manager.cpp
    src/common/diagnostic.cpp
    src/frontend/lexer/token_kind.cpp
    src/frontend/lexer/lexer.cpp
    src/frontend/preprocessor/preprocess_result.cpp
    src/frontend/preprocessor/include_resolver.cpp
    src/frontend/preprocessor/preprocessor.cpp
    src/frontend/parser/parser.cpp
    src/frontend/parser/parse_result.cpp
    src/frontend/syntax/syntax_kind.cpp
    src/frontend/syntax/syntax_tree.cpp
    src/frontend/ast/ast.cpp
    src/frontend/ast/ast_dump.cpp
    src/frontend/hir/hir.cpp
    src/frontend/hir/binding.cpp
    src/frontend/hir/hir_dump.cpp
    src/frontend/hir/typed.cpp
    src/frontend/hir/lower.cpp
    src/frontend/hir/eval.cpp
    src/ir/mir/lower.cpp
    src/ir/mir/ssa.cpp
    src/ir/mir/slice.cpp
    src/analysis/dominance.cpp
    src/ir/mir/verify.cpp
    src/ir/mir/dump.cpp
    src/ir/mir/eval.cpp
    src/runtime/scalar.cpp
    src/ad/forward.cpp
    src/ad/plan.cpp
    src/ad/parameters.cpp
    src/ad/sparsity.cpp
    src/ad/ir.cpp
    src/ad/prepared.cpp
    src/ad/state.cpp
    src/ad/eval.cpp
    src/validation/gradcheck.cpp
    src/codegen/c.cpp
)
target_include_directories(vagrad_frontend PUBLIC include)
target_compile_features(vagrad_frontend PUBLIC cxx_std_20)
set_target_properties(vagrad_frontend PROPERTIES CXX_EXTENSIONS OFF)

add_executable(vagrad tools/vagrad_main.cpp)
target_link_libraries(vagrad PRIVATE vagrad_frontend)
if(MINGW)
    target_link_options(vagrad PRIVATE -municode)
endif()

function(vagrad_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

vagrad_warnings(vagrad_frontend)
vagrad_warnings(vagrad)
option(VAGRAD_BUILD_EXAMPLES "Build the standalone forward AD teaching example" ON)
if(VAGRAD_BUILD_EXAMPLES)
    add_executable(vagrad_ad_demo examples/forward_ad_demo.cpp)
    target_link_libraries(vagrad_ad_demo PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_ad_demo)
    if(MINGW)
        target_link_options(vagrad_ad_demo PRIVATE -municode)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME cli_codegen_modes COMMAND "${CMAKE_COMMAND}"
        "-DVAGRAD_EXE=$<TARGET_FILE:vagrad>"
        "-DTEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata"
        "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-codegen"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_codegen_modes.cmake")
    foreach(fixture IN ITEMS mos math control)
        if(fixture STREQUAL "mos")
            set(cgen_input "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va")
            set(cgen_seeds --wrt G0 --wrt VTH0 --wrt TNOM)
        elseif(fixture STREQUAL "math")
            set(cgen_input "${CMAKE_CURRENT_SOURCE_DIR}/testdata/semantic/c_codegen_math.va")
            set(cgen_seeds --wrt P --wrt Q)
        else()
            set(cgen_input "${CMAKE_CURRENT_SOURCE_DIR}/testdata/semantic/c_codegen_control.va")
            set(cgen_seeds --wrt P)
        endif()
        set(cgen_source "${CMAKE_CURRENT_BINARY_DIR}/generated/${fixture}.c")
        add_custom_command(OUTPUT "${cgen_source}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
            COMMAND $<TARGET_FILE:vagrad> emit-c "${cgen_input}" ${cgen_seeds} -o "${cgen_source}"
            DEPENDS vagrad "${cgen_input}" "${CMAKE_CURRENT_SOURCE_DIR}/testdata/include/simple_mos_defaults.vams"
            VERBATIM)
        add_library(vagrad_cgen_${fixture} SHARED "${cgen_source}")
        set_target_properties(vagrad_cgen_${fixture} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        if(MSVC)
            target_compile_options(vagrad_cgen_${fixture} PRIVATE /utf-8 /fp:precise)
        endif()
        add_executable(vagrad_cgen_${fixture}_tests tests/c_codegen_tests.cpp)
        target_link_libraries(vagrad_cgen_${fixture}_tests PRIVATE vagrad_frontend vagrad_cgen_${fixture})
        vagrad_warnings(vagrad_cgen_${fixture}_tests)
        target_compile_definitions(vagrad_cgen_${fixture}_tests PRIVATE
            VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
            VAGRAD_CGEN_FILE="${cgen_input}")
        if(fixture STREQUAL "math")
            target_compile_definitions(vagrad_cgen_${fixture}_tests PRIVATE VAGRAD_CGEN_MATH=1)
        elseif(fixture STREQUAL "control")
            target_compile_definitions(vagrad_cgen_${fixture}_tests PRIVATE VAGRAD_CGEN_CONTROL=1)
        endif()
        add_test(NAME cgen_${fixture} COMMAND vagrad_cgen_${fixture}_tests)
    endforeach()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME python_kernel COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/python_kernel_tests.py"
            "$<TARGET_FILE:vagrad_cgen_mos>" "$<TARGET_FILE:vagrad_cgen_control>")
        set_tests_properties(python_kernel PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import numpy"
            RESULT_VARIABLE vagrad_numpy_status OUTPUT_QUIET ERROR_QUIET)
        if(vagrad_numpy_status EQUAL 0)
            add_test(NAME python_numpy COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/python_numpy_tests.py"
                "$<TARGET_FILE:vagrad_cgen_mos>" "$<TARGET_FILE:vagrad_cgen_control>")
            set_tests_properties(python_numpy PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
        endif()
    endif()
    if(VAGRAD_BUILD_EXAMPLES)
        add_test(NAME example_ad_batch COMMAND vagrad_ad_demo
            "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va")
        set_tests_properties(example_ad_batch PROPERTIES PASS_REGULAR_EXPRESSION "Vg,ids,d_ids_d_G0")
    endif()
    add_executable(vagrad_common_tests tests/common_tests.cpp)
    target_link_libraries(vagrad_common_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_common_tests)
    target_compile_definitions(vagrad_common_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    )
    add_test(NAME common COMMAND vagrad_common_tests)
    add_executable(vagrad_lexer_tests tests/lexer_tests.cpp)
    target_link_libraries(vagrad_lexer_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_lexer_tests)
    target_compile_definitions(vagrad_lexer_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    )
    add_test(NAME lexer COMMAND vagrad_lexer_tests)
    add_executable(vagrad_preprocessor_tests tests/preprocessor_tests.cpp)
    target_link_libraries(vagrad_preprocessor_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_preprocessor_tests)
    target_compile_definitions(vagrad_preprocessor_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    )
    add_test(NAME preprocessor COMMAND vagrad_preprocessor_tests)
    add_executable(vagrad_parser_tests tests/parser_tests.cpp)
    target_link_libraries(vagrad_parser_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_parser_tests)
    target_compile_definitions(vagrad_parser_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    )
    add_test(NAME parser COMMAND vagrad_parser_tests)
    add_executable(vagrad_ast_tests tests/ast_tests.cpp)
    target_link_libraries(vagrad_ast_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_ast_tests)
    target_compile_definitions(vagrad_ast_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    )
    add_test(NAME ast COMMAND vagrad_ast_tests)
    add_executable(vagrad_hir_tests tests/hir_tests.cpp)
    target_link_libraries(vagrad_hir_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_hir_tests)
    target_compile_definitions(vagrad_hir_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata"
    )
    add_test(NAME hir COMMAND vagrad_hir_tests)
    add_executable(vagrad_mir_tests tests/mir_tests.cpp)
    target_link_libraries(vagrad_mir_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_mir_tests)
    add_test(NAME mir COMMAND vagrad_mir_tests)
    add_executable(vagrad_ssa_tests tests/ssa_tests.cpp)
    target_link_libraries(vagrad_ssa_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_ssa_tests)
    target_compile_definitions(vagrad_ssa_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    add_test(NAME ssa COMMAND vagrad_ssa_tests)
    add_executable(vagrad_slice_tests tests/slice_tests.cpp)
    target_link_libraries(vagrad_slice_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_slice_tests)
    target_compile_definitions(vagrad_slice_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    add_test(NAME slice COMMAND vagrad_slice_tests)
    add_executable(vagrad_ad_tests tests/ad_tests.cpp)
    target_link_libraries(vagrad_ad_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_ad_tests)
    target_compile_definitions(vagrad_ad_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    add_test(NAME ad_primitives COMMAND vagrad_ad_tests)
    add_executable(vagrad_ad_rules_tests tests/ad_rules_tests.cpp)
    target_link_libraries(vagrad_ad_rules_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_ad_rules_tests)
    target_compile_definitions(vagrad_ad_rules_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    add_test(NAME ad_rules COMMAND vagrad_ad_rules_tests)
    add_executable(vagrad_ad_integration_tests tests/ad_integration_tests.cpp)
    target_link_libraries(vagrad_ad_integration_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_ad_integration_tests)
    target_compile_definitions(vagrad_ad_integration_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    add_test(NAME ad_integration COMMAND vagrad_ad_integration_tests)
    add_executable(vagrad_ad_parameter_tests tests/ad_parameter_tests.cpp)
    target_link_libraries(vagrad_ad_parameter_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_ad_parameter_tests)
    target_compile_definitions(vagrad_ad_parameter_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    add_test(NAME ad_parameters COMMAND vagrad_ad_parameter_tests)
    add_executable(vagrad_ad_ir_tests tests/ad_ir_tests.cpp)
    target_link_libraries(vagrad_ad_ir_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_ad_ir_tests)
    target_compile_definitions(vagrad_ad_ir_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    add_test(NAME ad_ir COMMAND vagrad_ad_ir_tests)
    add_executable(vagrad_ad_prepared_tests tests/ad_prepared_tests.cpp)
    target_link_libraries(vagrad_ad_prepared_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_ad_prepared_tests)
    target_compile_definitions(vagrad_ad_prepared_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    add_test(NAME ad_prepared COMMAND vagrad_ad_prepared_tests)
    add_executable(vagrad_gradcheck_tests tests/gradcheck_tests.cpp)
    target_link_libraries(vagrad_gradcheck_tests PRIVATE vagrad_frontend)
    vagrad_warnings(vagrad_gradcheck_tests)
    target_compile_definitions(vagrad_gradcheck_tests PRIVATE
        VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
    add_test(NAME gradcheck COMMAND vagrad_gradcheck_tests)
    add_test(NAME cli_gradcheck_modes COMMAND "${CMAKE_COMMAND}"
        "-DVAGRAD_EXE=$<TARGET_FILE:vagrad>"
        "-DTEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata"
        "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-gradcheck"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_gradcheck_modes.cmake")
    foreach(stage IN ITEMS verify eval)
        add_executable(vagrad_mir_${stage}_tests tests/mir_${stage}_tests.cpp)
        target_link_libraries(vagrad_mir_${stage}_tests PRIVATE vagrad_frontend)
        vagrad_warnings(vagrad_mir_${stage}_tests)
        target_compile_definitions(vagrad_mir_${stage}_tests PRIVATE
            VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
        add_test(NAME mir_${stage} COMMAND vagrad_mir_${stage}_tests)
    endforeach()
    foreach(stage IN ITEMS typed eval)
        add_executable(vagrad_hir_${stage}_tests tests/hir_${stage}_tests.cpp)
        target_link_libraries(vagrad_hir_${stage}_tests PRIVATE vagrad_frontend)
        vagrad_warnings(vagrad_hir_${stage}_tests)
        target_compile_definitions(vagrad_hir_${stage}_tests PRIVATE
            VAGRAD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
        add_test(NAME hir_${stage} COMMAND vagrad_hir_${stage}_tests)
    endforeach()
    add_test(NAME cli_source COMMAND vagrad source
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va")
    set_tests_properties(cli_source PROPERTIES PASS_REGULAR_EXPRESSION "FileId: 0")
    add_test(NAME cli_locate COMMAND vagrad locate
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va" 0)
    set_tests_properties(cli_locate PROPERTIES PASS_REGULAR_EXPRESSION "simple_mos.va:1:1")
    add_test(NAME cli_lex COMMAND vagrad lex
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va")
    set_tests_properties(cli_lex PROPERTIES
        PASS_REGULAR_EXPRESSION "1:1 \\[0,6\\) Keyword")
    add_test(NAME cli_preprocess COMMAND vagrad preprocess
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va")
    set_tests_properties(cli_preprocess PROPERTIES
        PASS_REGULAR_EXPRESSION "parameter real G0 = 1m")
    add_test(NAME cli_modes COMMAND "${CMAKE_COMMAND}"
        "-DVAGRAD_EXE=$<TARGET_FILE:vagrad>"
        "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-modes"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_modes.cmake")
    add_test(NAME cli_parse COMMAND vagrad parse
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va")
    set_tests_properties(cli_parse PROPERTIES PASS_REGULAR_EXPRESSION "ContributionStmt")
    add_test(NAME cli_parse_events COMMAND vagrad parse
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va" --dump-events)
    set_tests_properties(cli_parse_events PROPERTIES PASS_REGULAR_EXPRESSION "ForwardParent")
    add_test(NAME cli_parser_modes COMMAND "${CMAKE_COMMAND}"
        "-DVAGRAD_EXE=$<TARGET_FILE:vagrad>"
        "-DTEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata"
        "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-parser"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_parser_modes.cmake")
    add_test(NAME cli_ast COMMAND vagrad ast
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va")
    set_tests_properties(cli_ast PROPERTIES PASS_REGULAR_EXPRESSION "ModuleDecl name=.simple_mos.")
    add_test(NAME cli_ast_modes COMMAND "${CMAKE_COMMAND}"
        "-DVAGRAD_EXE=$<TARGET_FILE:vagrad>"
        "-DTEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata"
        "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-ast"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_ast_modes.cmake")
    add_test(NAME cli_hir COMMAND vagrad hir
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va")
    set_tests_properties(cli_hir PROPERTIES PASS_REGULAR_EXPRESSION "references=39 resolved=39 errors=0")
    add_test(NAME cli_hir_modes COMMAND "${CMAKE_COMMAND}"
        "-DVAGRAD_EXE=$<TARGET_FILE:vagrad>"
        "-DTEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata"
        "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-hir"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_hir_modes.cmake")
    add_test(NAME cli_hir_typed COMMAND vagrad hir
        "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va" --typed)
    set_tests_properties(cli_hir_typed PROPERTIES PASS_REGULAR_EXPRESSION "ContributeFlow")
    add_test(NAME cli_eval COMMAND vagrad eval "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va"
        --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300)
    set_tests_properties(cli_eval PROPERTIES PASS_REGULAR_EXPRESSION "HIR H3 status=ok")
    add_test(NAME cli_hir_eval_modes COMMAND "${CMAKE_COMMAND}"
        "-DVAGRAD_EXE=$<TARGET_FILE:vagrad>"
        "-DTEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata"
        "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-hir-eval"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_hir_eval_modes.cmake")
    add_test(NAME cli_mir COMMAND vagrad mir "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va" --form local)
    set_tests_properties(cli_mir PROPERTIES PASS_REGULAR_EXPRESSION "verified=true")
    add_test(NAME cli_cfg COMMAND vagrad cfg "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va")
    set_tests_properties(cli_cfg PROPERTIES PASS_REGULAR_EXPRESSION "reverse_postorder")
    add_test(NAME cli_mir_modes COMMAND "${CMAKE_COMMAND}"
        "-DVAGRAD_EXE=$<TARGET_FILE:vagrad>"
        "-DTEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata"
        "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-mir"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_mir_modes.cmake")
    add_test(NAME cli_ssa COMMAND vagrad mir "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va" --form ssa)
    set_tests_properties(cli_ssa PROPERTIES PASS_REGULAR_EXPRESSION "phi local")
    add_test(NAME cli_ssa_eval COMMAND vagrad eval "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va"
        --ir mir-ssa --voltage d=.05 --voltage g=1 --voltage s=0 --temperature 300)
    set_tests_properties(cli_ssa_eval PROPERTIES PASS_REGULAR_EXPRESSION "MIR-SSA status=ok")
    add_test(NAME cli_ssa_modes COMMAND "${CMAKE_COMMAND}"
        "-DVAGRAD_EXE=$<TARGET_FILE:vagrad>"
        "-DTEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata"
        "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-ssa"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_ssa_modes.cmake")
    add_test(NAME cli_slice COMMAND vagrad slice "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va" --target ids@exit --effects target-only)
    set_tests_properties(cli_slice PROPERTIES PASS_REGULAR_EXPRESSION "target_inputs")
    add_test(NAME cli_slice_eval COMMAND vagrad eval "${CMAKE_CURRENT_SOURCE_DIR}/testdata/simple_mos.va"
        --ir mir-slice --target ids@exit --effects target-only --voltage d=.05 --voltage g=1 --voltage s=0)
    set_tests_properties(cli_slice_eval PROPERTIES PASS_REGULAR_EXPRESSION "SSA-Slice status=ok")
    add_test(NAME cli_slice_modes COMMAND "${CMAKE_COMMAND}"
        "-DVAGRAD_EXE=$<TARGET_FILE:vagrad>"
        "-DTEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata"
        "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-slice"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_slice_modes.cmake")
    add_test(NAME cli_ad_modes COMMAND "${CMAKE_COMMAND}"
        "-DVAGRAD_EXE=$<TARGET_FILE:vagrad>"
        "-DTEST_DATA_DIR=${CMAKE_CURRENT_SOURCE_DIR}/testdata"
        "-DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-ad"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_ad_modes.cmake")
endif()

