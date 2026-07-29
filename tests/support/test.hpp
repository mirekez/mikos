#pragma once

#include <iostream>

namespace mikos::test {

class Suite {
 public:
  explicit Suite(const char* name) : name_{name} {}

  void check(bool condition, const char* expression) {
    if (!condition) {
      std::cerr << "FAIL: " << name_ << ": " << expression << '\n';
      ++failures_;
    }
  }

  [[nodiscard]] int finish() const {
    if (failures_ == 0) {
      std::cout << "PASS: " << name_ << '\n';
      return 0;
    }
    return 1;
  }

 private:
  const char* name_;
  int failures_{};
};

}  // namespace mikos::test

#define MIKOS_CHECK(suite, expression) (suite).check((expression), #expression)
