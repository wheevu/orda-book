#pragma once

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace testfw {

using TestFunc = std::function<void()>;

struct TestCase {
  std::string name;
  TestFunc func;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(std::string name, TestFunc func) {
    registry().push_back(TestCase{std::move(name), std::move(func)});
  }
};

inline void fail(const char* file, int line, const std::string& message) {
  std::ostringstream out;
  out << file << ':' << line << ": " << message;
  throw std::runtime_error(out.str());
}

template <typename Left, typename Right>
void check_equal(const Left& left, const Right& right, const char* left_expr, const char* right_expr,
                 const char* file, int line) {
  if (!(left == right)) {
    std::ostringstream out;
    out << "expected " << left_expr << " == " << right_expr << ", got [" << left << "] vs ["
        << right << ']';
    fail(file, line, out.str());
  }
}

inline void check_true(bool condition, const char* expr, const char* file, int line) {
  if (!condition) {
    std::ostringstream out;
    out << "expected true: " << expr;
    fail(file, line, out.str());
  }
}

}  // namespace testfw

#define TEST_CASE(name)                                                                                  \
  static void name();                                                                                    \
  static ::testfw::Registrar name##_registrar(#name, name);                                              \
  static void name()

#define CHECK_TRUE(expr) ::testfw::check_true((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(left, right) ::testfw::check_equal((left), (right), #left, #right, __FILE__, __LINE__)
