# 区分两个阶段：lex 不打开头文件，preprocess 统一处理所有预处理指令。
if(NOT DEFINED VAGRAD_EXE OR NOT DEFINED TEST_WORK_DIR)
    message(FATAL_ERROR "VAGRAD_EXE and TEST_WORK_DIR are required")
endif()

file(MAKE_DIRECTORY "${TEST_WORK_DIR}")
set(main "${TEST_WORK_DIR}/main.va")
set(header "${TEST_WORK_DIR}/child.vams")
set(missing "${TEST_WORK_DIR}/missing.va")
set(inactive "${TEST_WORK_DIR}/inactive.va")
set(tokens "${TEST_WORK_DIR}/merged.tokens.txt")
set(main_text "`include \"child.vams\"\nmodule test;\nparameter real GAIN = `GAIN;\nendmodule\n")
set(header_text "`define GAIN 2\nreal included;\n")
file(WRITE "${main}" "${main_text}")
file(WRITE "${header}" "${header_text}")
file(WRITE "${missing}" "`include \"not-provided.vams\"\nreal ok;\n")
file(WRITE "${inactive}" "`ifdef NOT_DEFINED\n`include \"not-provided.vams\"\n`endif\nreal ok;\n")

execute_process(COMMAND "${VAGRAD_EXE}" lex "${main}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
string(FIND "${output}" "Directive \"`include\"" directive)
string(FIND "${output}" "Identifier \"included\"" declaration)
if(NOT status EQUAL 0 OR directive EQUAL -1 OR NOT declaration EQUAL -1)
    message(FATAL_ERROR "lex must keep include as raw tokens without reading the header: ${errors}")
endif()

execute_process(COMMAND "${VAGRAD_EXE}" lex "${missing}"
    RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "lex must not require include files: ${errors}")
endif()

execute_process(COMMAND "${VAGRAD_EXE}" preprocess "${missing}"
    RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE errors)
if(NOT status EQUAL 1 OR NOT errors MATCHES "PP_INCLUDE_LOAD")
    message(FATAL_ERROR "preprocess must diagnose a missing active include")
endif()

execute_process(COMMAND "${VAGRAD_EXE}" preprocess "${inactive}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT status EQUAL 0 OR NOT output MATCHES "real ok")
    message(FATAL_ERROR "preprocess must skip an inactive include: ${errors}")
endif()

execute_process(COMMAND "${VAGRAD_EXE}" preprocess "${main}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT status EQUAL 0 OR NOT output MATCHES "real included;" OR
   NOT output MATCHES "parameter real GAIN = 2;")
    message(FATAL_ERROR "unified preprocess must include declarations and expand macros: ${errors}")
endif()

execute_process(COMMAND "${VAGRAD_EXE}" preprocess "${main}" --dump-tokens -o "${tokens}"
    RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "cannot save unified token output: ${errors}")
endif()
file(READ "${tokens}" output)
string(FIND "${output}" "File[1]" header_file)
string(FIND "${output}" "Macro \"GAIN\"" macro_context)
string(FIND "${output}" " Directive " remaining_directive)
if(header_file EQUAL -1 OR macro_context EQUAL -1 OR NOT remaining_directive EQUAL -1)
    message(FATAL_ERROR "token dump must contain header and macro origins, with directives processed")
endif()

# 两个入口均防止覆盖自己实际读取的输入。
execute_process(COMMAND "${VAGRAD_EXE}" lex "${main}" -o "${main}"
    RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE errors)
file(READ "${main}" after)
if(NOT status EQUAL 1 OR NOT errors MATCHES "CLI_OUTPUT" OR NOT after STREQUAL main_text)
    message(FATAL_ERROR "lex input protection failed")
endif()
execute_process(COMMAND "${VAGRAD_EXE}" preprocess "${main}" -o "${header}"
    RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE errors)
file(READ "${header}" after)
if(NOT status EQUAL 1 OR NOT errors MATCHES "CLI_OUTPUT" OR NOT after STREQUAL header_text)
    message(FATAL_ERROR "preprocessor header protection failed")
endif()
