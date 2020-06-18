// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#include <mpark/variant.hpp>

#include <iostream>
#include <string>

#include <gtest/gtest.h>

TEST(Variant, Intro) {
  // direct initialization.
  mpark::variant<int, std::string> v("hello world!");

  // direct access via reference.
  EXPECT_EQ("hello world!", mpark::get<std::string>(v));

  // bad access.
#ifdef MPARK_EXCEPTIONS
  EXPECT_THROW(mpark::get<int>(v), mpark::bad_variant_access);
#endif

  // copy construction.
  mpark::variant<int, std::string> w(v);

  // direct access via pointer.
  EXPECT_FALSE(mpark::get_if<int>(&w));
  EXPECT_TRUE(mpark::get_if<std::string>(&w));

  // diff-type assignment.
  v = 42;

  struct unary {
    int operator()(int) const noexcept { return 0; }
    int operator()(const std::string &) const noexcept { return 1; }
  };  // unary

  // single visitation.
  EXPECT_EQ(0, mpark::visit(unary{}, v));

  // same-type assignment.
  w = "hello";

  EXPECT_NE(v, w);

  // make `w` equal to `v`.
  w = 42;

  EXPECT_EQ(v, w);

  struct binary {
    int operator()(int, int) const noexcept { return 0; }
    int operator()(int, const std::string &) const noexcept { return 1; }
    int operator()(const std::string &, int) const noexcept { return 2; }
    int operator()(const std::string &, const std::string &) const noexcept {
      return 3;
    }
  };  // binary

  // binary visitation.
  EXPECT_EQ(0, mpark::visit(binary{}, v, w));
}
