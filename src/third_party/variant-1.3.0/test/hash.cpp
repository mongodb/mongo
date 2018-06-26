// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#include <mpark/variant.hpp>

#include <string>

#include <gtest/gtest.h>

TEST(Hash, Monostate) {
  mpark::variant<int, mpark::monostate, std::string> v(mpark::monostate{});
  // Construct hash function objects.
  std::hash<mpark::monostate> monostate_hash;
  std::hash<mpark::variant<int, mpark::monostate, std::string>> variant_hash;
  // Check the hash.
  EXPECT_NE(monostate_hash(mpark::monostate{}), variant_hash(v));
}

TEST(Hash, String) {
  mpark::variant<int, std::string> v("hello");
  EXPECT_EQ("hello", mpark::get<std::string>(v));
  // Construct hash function objects.
  std::hash<std::string> string_hash;
  std::hash<mpark::variant<int, std::string>> variant_hash;
  // Check the hash.
  EXPECT_NE(string_hash("hello"), variant_hash(v));
}
