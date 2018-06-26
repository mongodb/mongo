// MPark.Variant
//
// Copyright Michael Park, 2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#include <iostream>
#include <string>
#include <vector>

#include <mpark/variant.hpp>

int main() {
  std::vector<mpark::variant<int, std::string>> vs = { 101, "+", 202, "==", 303 };
  for (const auto& v : vs) {
    mpark::visit([](const auto& x) { std::cout << x << ' '; }, v);
  }
}
