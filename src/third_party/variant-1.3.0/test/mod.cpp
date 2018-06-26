// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#include <mpark/variant.hpp>

#include <string>

#include <gtest/gtest.h>

TEST(Assign_Emplace, IndexDirect) {
  mpark::variant<int, std::string> v;
  v.emplace<1>("42");
  EXPECT_EQ("42", mpark::get<1>(v));
}

TEST(Assign_Emplace, IndexDirectDuplicate) {
  mpark::variant<int, int> v;
  v.emplace<1>(42);
  EXPECT_EQ(42, mpark::get<1>(v));
}

TEST(Assign_Emplace, IndexConversion) {
  mpark::variant<int, std::string> v;
  v.emplace<1>("42");
  EXPECT_EQ("42", mpark::get<1>(v));
}

TEST(Assign_Emplace, IndexConversionDuplicate) {
  mpark::variant<int, int> v;
  v.emplace<1>(1.1);
  EXPECT_EQ(1, mpark::get<1>(v));
}

TEST(Assign_Emplace, IndexInitializerList) {
  mpark::variant<int, std::string> v;
  v.emplace<1>({'4', '2'});
  EXPECT_EQ("42", mpark::get<1>(v));
}

TEST(Assign_Emplace, TypeDirect) {
  mpark::variant<int, std::string> v;
  v.emplace<std::string>("42");
  EXPECT_EQ("42", mpark::get<std::string>(v));
}

TEST(Assign_Emplace, TypeConversion) {
  mpark::variant<int, std::string> v;
  v.emplace<int>(1.1);
  EXPECT_EQ(1, mpark::get<int>(v));
}

TEST(Assign_Emplace, TypeInitializerList) {
  mpark::variant<int, std::string> v;
  v.emplace<std::string>({'4', '2'});
  EXPECT_EQ("42", mpark::get<std::string>(v));
}
