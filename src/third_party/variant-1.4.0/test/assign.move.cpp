// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#include <mpark/variant.hpp>

#include <gtest/gtest.h>

#include "util.hpp"

namespace lib = mpark::lib;

TEST(Assign_Move, SameType) {
  struct Obj {
    constexpr Obj() {}
    Obj(const Obj &) = delete;
    Obj(Obj &&) noexcept { EXPECT_TRUE(false); }
    Obj &operator=(const Obj &) = delete;
    Obj &operator=(Obj &&) noexcept { EXPECT_TRUE(true); return *this; }
  };
  // `v`, `w`.
  mpark::variant<Obj, int> v, w;
  // move assignment.
  v = lib::move(w);
}

TEST(Assign_Move, DiffType) {
  struct Obj {
    constexpr Obj() {}
    Obj(const Obj &) = delete;
    Obj(Obj &&) noexcept { EXPECT_TRUE(true); }
    Obj &operator=(const Obj &) = delete;
    Obj &operator=(Obj &&) noexcept { EXPECT_TRUE(false); return *this; }
  };
  // `v`, `w`.
  mpark::variant<Obj, int> v(42), w;
  // move assignment.
  v = lib::move(w);
}

#ifdef MPARK_EXCEPTIONS
TEST(Assign_Move, ValuelessByException) {
  mpark::variant<int, move_thrower_t> v(42);
  EXPECT_THROW(v = move_thrower_t{}, MoveConstruction);
  EXPECT_TRUE(v.valueless_by_exception());
  mpark::variant<int, move_thrower_t> w(42);
  w = lib::move(v);
  EXPECT_TRUE(w.valueless_by_exception());
}
#endif
