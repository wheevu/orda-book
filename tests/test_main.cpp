#include "test_framework.hpp"

#include <exception>
#include <iostream>

int main() {
  int failures = 0;
  for (const auto& test : testfw::registry()) {
    try {
      test.func();
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception& ex) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << " - " << ex.what() << '\n';
    }
  }

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }

  std::cout << "All tests passed: " << testfw::registry().size() << '\n';
  return 0;
}
