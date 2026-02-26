set(script)

function(add_command NAME)
  set(_args "")
  foreach(_arg ${ARGN})
    set(_args "${_args} [==[${_arg}]==]")
  endforeach()
  set(script "${script}${NAME}(${_args})\n" PARENT_SCOPE)
endfunction()

if(NOT EXISTS "${TEST_EXECUTABLE}")
  message(FATAL_ERROR "Executable does not exist: ${TEST_EXECUTABLE}")
endif()
execute_process(
  COMMAND "${TEST_EXECUTABLE}" list-tests
  WORKING_DIRECTORY "${TEST_WORKING_DIR}"
  OUTPUT_VARIABLE output
  RESULT_VARIABLE result
)
if(NOT ${result} EQUAL 0)
  message(FATAL_ERROR "Error running executable: ${TEST_EXECUTABLE}")
endif()

string(REPLACE "\n" ";" output "${output}")

foreach(line ${output})
  set(test "${line}")
  add_command(add_test
    "rnp_tests-${test}"
    "${TEST_EXECUTABLE}"
    "${test}"
  )
  add_command(set_tests_properties
    "rnp_tests-${test}"
    PROPERTIES ${TEST_PROPERTIES}
  )
endforeach()

file(WRITE "${CTEST_FILE}" "${script}")

