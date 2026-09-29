# Configures, builds, and runs the out-of-tree consumer against an installed
# prefix. Used by the package-consumption test.
if(NOT DEFINED TEM_CONSUMER_SOURCE OR NOT DEFINED TEM_CONSUMER_PREFIX OR NOT DEFINED TEM_CONSUMER_BINARY)
  message(FATAL_ERROR "TEM_CONSUMER_SOURCE, TEM_CONSUMER_PREFIX and TEM_CONSUMER_BINARY are required")
endif()

file(REMOVE_RECURSE "${TEM_CONSUMER_BINARY}")
file(MAKE_DIRECTORY "${TEM_CONSUMER_BINARY}")

set(configure_args
  -S "${TEM_CONSUMER_SOURCE}"
  -B "${TEM_CONSUMER_BINARY}"
  "-DCMAKE_PREFIX_PATH=${TEM_CONSUMER_PREFIX}")

if(DEFINED TEM_CONSUMER_CONFIG AND NOT TEM_CONSUMER_CONFIG STREQUAL "")
  list(APPEND configure_args --config "${TEM_CONSUMER_CONFIG}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" ${configure_args}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "consumer configure failed:\n${configure_output}\n${configure_error}")
endif()

set(build_args --build "${TEM_CONSUMER_BINARY}")
if(DEFINED TEM_CONSUMER_CONFIG AND NOT TEM_CONSUMER_CONFIG STREQUAL "")
  list(APPEND build_args --config "${TEM_CONSUMER_CONFIG}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" ${build_args}
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "consumer build failed:\n${build_output}\n${build_error}")
endif()

find_program(TEM_CONSUMER_EXECUTABLE
  NAMES tem_consumer tem_consumer.exe
  PATHS "${TEM_CONSUMER_BINARY}/Release" "${TEM_CONSUMER_BINARY}/Debug" "${TEM_CONSUMER_BINARY}"
  NO_DEFAULT_PATH)
if(NOT TEM_CONSUMER_EXECUTABLE)
  message(FATAL_ERROR "consumer executable was not produced in ${TEM_CONSUMER_BINARY}")
endif()

execute_process(
  COMMAND "${TEM_CONSUMER_EXECUTABLE}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
message(STATUS "consumer output:\n${run_output}")
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "consumer run failed (${run_result}):\n${run_error}")
endif()
