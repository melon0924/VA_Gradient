if(NOT DEFINED VAGRAD_EXE OR NOT DEFINED TEST_DATA_DIR OR NOT DEFINED TEST_WORK_DIR)
    message(FATAL_ERROR "VAGRAD_EXE, TEST_DATA_DIR and TEST_WORK_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_WORK_DIR}")
set(main "${TEST_DATA_DIR}/simple_mos.va")
set(header "${TEST_DATA_DIR}/include/simple_mos_defaults.vams")
set(broken "${TEST_DATA_DIR}/parser_errors.va")
set(events "${TEST_WORK_DIR}/model.events.txt")
set(syntax "${TEST_WORK_DIR}/broken.cst.txt")

execute_process(COMMAND "${VAGRAD_EXE}" parse "${main}" -DSOFT_THRESHOLD -D TRACE_MOS
    --dump-events -o "${events}" RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "parse must reuse preprocessing options: ${errors}")
endif()
file(READ "${events}" output)
if(NOT output MATCHES "StartNode" OR NOT output MATCHES "ForwardParent" OR
   NOT output MATCHES "OriginContext" OR NOT output MATCHES "sqrt")
    message(FATAL_ERROR "event output must preserve expressions and source map")
endif()

execute_process(COMMAND "${VAGRAD_EXE}" parse "${broken}" -o "${syntax}"
    RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 1 OR NOT errors MATCHES "PARSE_EXPRESSION" OR
   NOT errors MATCHES "expected ';'")
    message(FATAL_ERROR "invalid syntax must report both errors: ${errors}")
endif()
file(READ "${syntax}" output)
if(NOT output MATCHES "ErrorNode" OR NOT output MATCHES "ContributionStmt")
    message(FATAL_ERROR "CST must preserve errors and later valid statements")
endif()

file(READ "${header}" before)
execute_process(COMMAND "${VAGRAD_EXE}" parse "${main}" -o "${header}"
    RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE errors)
file(READ "${header}" after)
if(NOT status EQUAL 1 OR NOT errors MATCHES "CLI_OUTPUT" OR NOT before STREQUAL after)
    message(FATAL_ERROR "parse output must not overwrite an included input file")
endif()

execute_process(COMMAND "${VAGRAD_EXE}" parse "${main}" --dump-tokens
    RESULT_VARIABLE status OUTPUT_QUIET ERROR_QUIET)
if(NOT status EQUAL 2)
    message(FATAL_ERROR "parse must reject options belonging to other stages")
endif()
