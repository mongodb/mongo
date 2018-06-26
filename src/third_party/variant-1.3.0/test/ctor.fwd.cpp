// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#include <mpark/variant.hpp>

#include <string>

#include <gtest/gtest.h>

TEST(Ctor_Fwd, Direct) {
  mpark::variant<int, std::string> v(42);
  EXPECT_EQ(42, mpark::get<int>(v));

  /* constexpr */ {
    constexpr mpark::variant<int, const char *> cv(42);
    static_assert(42 == mpark::get<int>(cv), "");
  }
}

TEST(Ctor_Fwd, DirectConversion) {
  mpark::variant<int, std::string> v("42");
  EXPECT_EQ("42", mpark::get<std::string>(v));

  /* constexpr */ {
    constexpr mpark::variant<int, const char *> cv(1.1);
    static_assert(1 == mpark::get<int>(cv), "");
  }
}

TEST(Ctor_Fwd, CopyInitialization) {
  mpark::variant<int, std::string> v = 42;
  EXPECT_EQ(42, mpark::get<int>(v));

  /* constexpr */ {
    constexpr mpark::variant<int, const char *> cv = 42;
    static_assert(42 == mpark::get<int>(cv), "");
  }
}

TEST(Ctor_Fwd, CopyInitializationConversion) {
  mpark::variant<int, std::string> v = "42";
  EXPECT_EQ("42", mpark::get<std::string>(v));

  /* constexpr */ {
    constexpr mpark::variant<int, const char *> cv = 1.1;
    static_assert(1 == mpark::get<int>(cv), "");
  }
}
