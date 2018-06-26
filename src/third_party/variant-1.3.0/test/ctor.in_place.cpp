// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#include <mpark/variant.hpp>

#include <string>

#include <gtest/gtest.h>

TEST(Ctor_InPlace, IndexDirect) {
  mpark::variant<int, std::string> v(mpark::in_place_index_t<0>{}, 42);
  EXPECT_EQ(42, mpark::get<0>(v));

  /* constexpr */ {
    constexpr mpark::variant<int, const char *> cv(mpark::in_place_index_t<0>{},
                                                   42);
    static_assert(42 == mpark::get<0>(cv), "");
  }
}

TEST(Ctor_InPlace, IndexDirectDuplicate) {
  mpark::variant<int, int> v(mpark::in_place_index_t<0>{}, 42);
  EXPECT_EQ(42, mpark::get<0>(v));

  /* constexpr */ {
    constexpr mpark::variant<int, int> cv(mpark::in_place_index_t<0>{}, 42);
    static_assert(42 == mpark::get<0>(cv), "");
  }
}

TEST(Ctor_InPlace, IndexConversion) {
  mpark::variant<int, std::string> v(mpark::in_place_index_t<1>{}, "42");
  EXPECT_EQ("42", mpark::get<1>(v));

  /* constexpr */ {
    constexpr mpark::variant<int, const char *> cv(mpark::in_place_index_t<0>{},
                                                   1.1);
    static_assert(1 == mpark::get<0>(cv), "");
  }
}

TEST(Ctor_InPlace, IndexInitializerList) {
  mpark::variant<int, std::string> v(mpark::in_place_index_t<1>{}, {'4', '2'});
  EXPECT_EQ("42", mpark::get<1>(v));
}

TEST(Ctor_InPlace, TypeDirect) {
  mpark::variant<int, std::string> v(mpark::in_place_type_t<std::string>{},
                                     "42");
  EXPECT_EQ("42", mpark::get<std::string>(v));

  /* constexpr */ {
    constexpr mpark::variant<int, const char *> cv(
        mpark::in_place_type_t<int>{}, 42);
    static_assert(42 == mpark::get<int>(cv), "");
  }
}

TEST(Ctor_InPlace, TypeConversion) {
  mpark::variant<int, std::string> v(mpark::in_place_type_t<int>{}, 42.5);
  EXPECT_EQ(42, mpark::get<int>(v));

  /* constexpr */ {
    constexpr mpark::variant<int, const char *> cv(
        mpark::in_place_type_t<int>{}, 42.5);
    static_assert(42 == mpark::get<int>(cv), "");
  }
}

TEST(Ctor_InPlace, TypeInitializerList) {
  mpark::variant<int, std::string> v(mpark::in_place_type_t<std::string>{},
                                     {'4', '2'});
  EXPECT_EQ("42", mpark::get<std::string>(v));
}
