cmake_minimum_required(VERSION 3.25)

project(ThermalEmergencyManager
  VERSION 1.0.0
  DESCRIPTION "Thermal emergency coordination for data center facilities"
  HOMEPAGE_URL "https://github.com/summonlabs/Thermal-Emergency-Manager"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(TEM_IS_TOP_LEVEL ON)
else()
  set(TEM_IS_TOP_LEVEL OFF)
endif()

option(TEM_BUILD_TESTS "Build the test suite" ${TEM_IS_TOP_LEVEL})
option(TEM_BUILD_TOOLS "Build the administration CLI" ON)
option(TEM_BUILD_EXAMPLES "Build examples" ON)
option(TEM_BUILD_BENCHMARKS "Build the benchmark" ON)
option(TEM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(TEM_ENABLE_ASAN "Build with AddressSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

function(tem_strict_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(TEM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(TEM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

if(TEM_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
  endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(tem
  src/apply.cpp
  src/authority.cpp
  src/clock.cpp
  src/codec.cpp
  src/evidence.cpp
  src/ids.cpp
  src/journal.cpp
  src/mitigation.cpp
  src/obligations.cpp
  src/plan.cpp
  src/policy.cpp
  src/recovery.cpp
  src/refs.cpp
  src/report.cpp
  src/runtime.cpp
  src/scenario.cpp
  src/serialization.cpp
  src/severity.cpp
  src/state.cpp
  src/status.cpp
  src/store.cpp
  src/transport.cpp
  src/units.cpp
  src/version.cpp
)

add_library(Summon::tem ALIAS tem)

set_target_properties(tem PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(tem
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(tem PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(tem PRIVATE Threads::Threads)

tem_strict_options(tem)

# ---------------------------------------------------------------------------
# Administration CLI, example, benchmark
# ---------------------------------------------------------------------------

if(TEM_BUILD_TOOLS)
  add_executable(temctl tools/temctl.cpp)
  target_link_libraries(temctl PRIVATE Summon::tem)
  tem_strict_options(temctl)
endif()

if(TEM_BUILD_EXAMPLES)
  add_executable(tem_example_thermal_excursion examples/thermal_excursion.cpp)
  target_link_libraries(tem_example_thermal_excursion PRIVATE Summon::tem)
  tem_strict_options(tem_example_thermal_excursion)
endif()

if(TEM_BUILD_BENCHMARKS)
  add_executable(tem_bench bench/tem_bench.cpp)
  target_link_libraries(tem_bench PRIVATE Summon::tem)
  tem_strict_options(tem_bench)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(TEM_BUILD_TESTS)
  enable_testing()

  add_executable(tem_test_child tests/test_child.cpp)
  target_link_libraries(tem_test_child PRIVATE Summon::tem)
  tem_strict_options(tem_test_child)

  function(tem_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Summon::tem)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(${name} PRIVATE
      TEM_TEST_CHILD_PATH="$<TARGET_FILE:tem_test_child>")
    tem_strict_options(${name})
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  tem_add_test(tem_unit_tests tests/unit_tests.cpp)
  tem_add_test(tem_state_machine_tests tests/state_machine_tests.cpp)
  tem_add_test(tem_persistence_tests tests/persistence_tests.cpp)
  tem_add_test(tem_process_tests tests/process_tests.cpp)
  tem_add_test(tem_scenario_tests tests/scenario_tests.cpp)
  tem_add_test(tem_adversarial_tests tests/adversarial_tests.cpp)
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------

install(TARGETS tem
  EXPORT ThermalEmergencyManagerTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(TEM_BUILD_TOOLS)
  install(TARGETS temctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT ThermalEmergencyManagerTargets
  FILE ThermalEmergencyManagerTargets.cmake
  NAMESPACE Summon::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThermalEmergencyManager)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThermalEmergencyManagerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ThermalEmergencyManagerConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThermalEmergencyManager)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ThermalEmergencyManagerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ThermalEmergencyManagerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ThermalEmergencyManagerConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThermalEmergencyManager)

install(FILES README.md LICENSE NOTICE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ThermalEmergencyManager)

# ---------------------------------------------------------------------------
# Optional out-of-tree consumer validation against an installed prefix
# ---------------------------------------------------------------------------

if(TEM_BUILD_TESTS AND TEM_CONSUMER_PREFIX)
  add_test(NAME tem_package_consumer
    COMMAND ${CMAKE_COMMAND}
      -DTEM_CONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer
      -DTEM_CONSUMER_PREFIX=${TEM_CONSUMER_PREFIX}
      -DTEM_CONSUMER_BINARY=${CMAKE_CURRENT_BINARY_DIR}/consumer-build
      -DTEM_CONSUMER_CONFIG=$<CONFIG>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunConsumer.cmake)
endif()
