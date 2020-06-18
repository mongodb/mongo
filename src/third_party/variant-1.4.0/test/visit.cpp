// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#include <mpark/variant.hpp>

#include <string>
#include <sstream>

#include <gtest/gtest.h>

#include <mpark/config.hpp>

#include "util.hpp"

namespace lib = mpark::lib;

TEST(Visit, MutVarMutType) {
  mpark::variant<int> v(42);
  // Check `v`.
  EXPECT_EQ(42, mpark::get<int>(v));
  // Check qualifier.
  EXPECT_EQ(LRef, mpark::visit(get_qual, v));
  EXPECT_EQ(RRef, mpark::visit(get_qual, lib::move(v)));
}

TEST(Visit, MutVarConstType) {
  mpark::variant<const int> v(42);
  EXPECT_EQ(42, mpark::get<const int>(v));
  // Check qualifier.
  EXPECT_EQ(ConstLRef, mpark::visit(get_qual, v));
  EXPECT_EQ(ConstRRef, mpark::visit(get_qual, lib::move(v)));
}

TEST(Visit, ConstVarMutType) {
  const mpark::variant<int> v(42);
  EXPECT_EQ(42, mpark::get<int>(v));
  // Check qualifier.
  EXPECT_EQ(ConstLRef, mpark::visit(get_qual, v));
  EXPECT_EQ(ConstRRef, mpark::visit(get_qual, lib::move(v)));

#ifdef MPARK_CPP11_CONSTEXPR
  /* constexpr */ {
    constexpr mpark::variant<int> cv(42);
    static_assert(42 == mpark::get<int>(cv), "");
    // Check qualifier.
    static_assert(ConstLRef == mpark::visit(get_qual, cv), "");
    static_assert(ConstRRef == mpark::visit(get_qual, lib::move(cv)), "");
  }
#endif
}

TEST(Visit, ConstVarConstType) {
  const mpark::variant<const int> v(42);
  EXPECT_EQ(42, mpark::get<const int>(v));
  // Check qualifier.
  EXPECT_EQ(ConstLRef, mpark::visit(get_qual, v));
  EXPECT_EQ(ConstRRef, mpark::visit(get_qual, lib::move(v)));

#ifdef MPARK_CPP11_CONSTEXPR
  /* constexpr */ {
    constexpr mpark::variant<const int> cv(42);
    static_assert(42 == mpark::get<const int>(cv), "");
    // Check qualifier.
    static_assert(ConstLRef == mpark::visit(get_qual, cv), "");
    static_assert(ConstRRef == mpark::visit(get_qual, lib::move(cv)), "");
  }
#endif
}

struct concat {
  template <typename... Args>
  std::string operator()(const Args &... args) const {
    std::ostringstream strm;
    std::initializer_list<int>({(strm << args, 0)...});
    return lib::move(strm).str();
  }
};

TEST(Visit, Zero) { EXPECT_EQ("", mpark::visit(concat{})); }

TEST(Visit_Homogeneous, Double) {
  mpark::variant<int, std::string> v("hello"), w("world!");
  EXPECT_EQ("helloworld!", mpark::visit(concat{}, v, w));

#ifdef MPARK_CPP11_CONSTEXPR
  /* constexpr */ {
    constexpr mpark::variant<int, double> cv(101), cw(202), cx(3.3);
    struct add_ints {
      constexpr int operator()(int lhs, int rhs) const { return lhs + rhs; }
      constexpr int operator()(int lhs, double) const { return lhs; }
      constexpr int operator()(double, int rhs) const { return rhs; }
      constexpr int operator()(double, double) const { return 0; }
    };  // add
    static_assert(303 == mpark::visit(add_ints{}, cv, cw), "");
    static_assert(202 == mpark::visit(add_ints{}, cw, cx), "");
    static_assert(101 == mpark::visit(add_ints{}, cx, cv), "");
    static_assert(0 == mpark::visit(add_ints{}, cx, cx), "");
  }
#endif
}

TEST(Visit_Homogeneous, Quintuple) {
  mpark::variant<int, std::string> v(101), w("+"), x(202), y("="), z(303);
  EXPECT_EQ("101+202=303", mpark::visit(concat{}, v, w, x, y, z));
}

TEST(Visit_Heterogeneous, Double) {
  mpark::variant<int, std::string> v("hello");
  mpark::variant<double, const char *> w("world!");
  EXPECT_EQ("helloworld!", mpark::visit(concat{}, v, w));
}

TEST(Visit_Heterogenous, Quintuple) {
  mpark::variant<int, double> v(101);
  mpark::variant<const char *> w("+");
  mpark::variant<bool, std::string, int> x(202);
  mpark::variant<char, std::string, const char *> y('=');
  mpark::variant<long, short> z(303L);
  EXPECT_EQ("101+202=303", mpark::visit(concat{}, v, w, x, y, z));
}
