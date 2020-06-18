// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#include <mpark/variant.hpp>

#include <map>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#ifdef MPARK_INCOMPLETE_TYPE_TRAITS
// https://github.com/mpark/variant/issues/34
TEST(Issue, 34) {
  struct S {
    S(const S &) = default;
    S(S &&) = default;
    S &operator=(const S &) = default;
    S &operator=(S &&) = default;

    mpark::variant<std::map<std::string, S>> value;
  };
}
#endif

// https://github.com/mpark/variant/pull/57
TEST(Issue, 57) {
  std::vector<mpark::variant<int, std::unique_ptr<int>>> vec;
  vec.emplace_back(0);
}
