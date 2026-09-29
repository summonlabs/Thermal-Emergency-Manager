// Thermal Emergency Manager -- minimal first-party test harness.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Deliberately dependency-free: the repository takes no third-party test
// dependency, and the harness is small enough to audit. Tests are proof
// obligations, so a failure reports the exact expression, file, and line.
#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "tem/status.hpp"

namespace temtest {

struct TestCase {
  std::string name;
  void (*function)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

inline int& failure_count() {
  static int count = 0;
  return count;
}

inline std::vector<std::string>& current_failures() {
  static std::vector<std::string> failures;
  return failures;
}

class RequirementFailed : public std::exception {
 public:
  explicit RequirementFailed(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

class Registrar {
 public:
  Registrar(const char* name, void (*function)()) { registry().push_back(TestCase{name, function}); }
};

// Overloads so a status-shaped assertion can describe either a Status or any
// Result<T> without calling status() on a successful result.
template <class T>
[[nodiscard]] inline std::string failure_text(const T& value) {
  return value.status().to_string();
}

[[nodiscard]] inline std::string failure_text(const ::summon::tem::Status& value) {
  return value.to_string();
}

inline void report_failure(const char* file, int line, const std::string& text) {
  std::ostringstream stream;
  stream << file << ":" << line << ": " << text;
  current_failures().push_back(stream.str());
  ++failure_count();
}

inline int run_all(int argc, char** argv) {
  std::string filter;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--list") {
      list = true;
    } else if (arg.rfind("--filter=", 0) == 0) {
      filter = arg.substr(9);
    } else if (arg == "--help") {
      std::cout << "usage: <test-binary> [--list] [--filter=SUBSTRING]\n";
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return 2;
    }
  }
  if (list) {
    for (const TestCase& test : registry()) {
      std::cout << test.name << "\n";
    }
    return 0;
  }

  int executed = 0;
  int failed_tests = 0;
  for (const TestCase& test : registry()) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      continue;
    }
    current_failures().clear();
    const int before = failure_count();
    ++executed;
    std::cout << "[ RUN  ] " << test.name << "\n";
    try {
      test.function();
    } catch (const RequirementFailed& failure) {
      report_failure("<require>", 0, failure.what());
    } catch (const std::exception& error) {
      report_failure("<exception>", 0, error.what());
    } catch (...) {
      report_failure("<exception>", 0, "non-standard exception");
    }
    if (failure_count() != before) {
      ++failed_tests;
      std::cout << "[ FAIL ] " << test.name << "\n";
      for (const std::string& message : current_failures()) {
        std::cout << "         " << message << "\n";
      }
    } else {
      std::cout << "[  OK  ] " << test.name << "\n";
    }
  }
  std::cout << (failed_tests == 0 ? "PASSED " : "FAILED ") << executed - failed_tests << "/"
            << executed << " tests\n";
  return failed_tests == 0 ? 0 : 1;
}

}  // namespace temtest

#define TEM_TEST(name)                                                       \
  static void name();                                                        \
  static const ::temtest::Registrar tem_registrar_##name(#name, &name);      \
  static void name()

#define TEM_CHECK(condition)                                                          \
  do {                                                                                \
    if (!(condition)) {                                                               \
      ::temtest::report_failure(__FILE__, __LINE__, "expected: " #condition);         \
    }                                                                                 \
  } while (false)

#define TEM_REQUIRE(condition)                                                        \
  do {                                                                                \
    if (!(condition)) {                                                               \
      throw ::temtest::RequirementFailed(std::string(__FILE__) + ":" +                \
                                         std::to_string(__LINE__) + ": require " #condition); \
    }                                                                                 \
  } while (false)

// Requires a Status/Result-shaped value to be ok and reports the machine code
// and message when it is not.
#define TEM_REQUIRE_OK(expression)                                                       \
  do {                                                                                   \
    const auto& tem_result = (expression);                                               \
    if (!tem_result.ok()) {                                                              \
      throw ::temtest::RequirementFailed(std::string(__FILE__) + ":" +                    \
                                         std::to_string(__LINE__) + ": " #expression +   \
                                         " -> " + ::temtest::failure_text(tem_result));  \
    }                                                                                    \
  } while (false)

#define TEM_CHECK_EQ(expected, actual)                                                \
  do {                                                                                \
    const auto& tem_expected = (expected);                                            \
    const auto& tem_actual = (actual);                                                \
    if (!(tem_expected == tem_actual)) {                                              \
      std::ostringstream tem_stream;                                                  \
      tem_stream << "expected " #expected " == " #actual;                             \
      ::temtest::report_failure(__FILE__, __LINE__, tem_stream.str());                \
    }                                                                                 \
  } while (false)

#define TEM_FAIL(message)                                                             \
  do {                                                                                \
    std::ostringstream tem_stream;                                                    \
    tem_stream << message;                                                            \
    ::temtest::report_failure(__FILE__, __LINE__, tem_stream.str());                  \
  } while (false)

#define TEM_TEST_MAIN()                                     \
  int main(int argc, char** argv) { return ::temtest::run_all(argc, argv); }
