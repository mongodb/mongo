// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#include <mpark/variant.hpp>

#include <gtest/gtest.h>

#include "util.hpp"

TEST(Assign_Copy, SameType) {
  struct Obj {
    constexpr Obj() {}
    Obj(const Obj &) noexcept { EXPECT_TRUE(false); }
    Obj(Obj &&) = default;
    Obj &operator=(const Obj &) noexcept { EXPECT_TRUE(true); return *this; }
    Obj &operator=(Obj &&) = delete;
  };
  // `v`, `w`.
  mpark::variant<Obj, int> v, w;
  // copy assignment.
  v = w;
}

TEST(Assign_Copy, DiffType) {
  struct Obj {
    constexpr Obj() {}
    Obj(const Obj &) noexcept { EXPECT_TRUE(true); }
    Obj(Obj &&) = default;
    Obj &operator=(const Obj &) noexcept { EXPECT_TRUE(false); return *this; }
    Obj &operator=(Obj &&) = delete;
  };
  // `v`, `w`.
  mpark::variant<Obj, int> v(42), w;
  // copy assignment.
  v = w;
}

#ifdef MPARK_EXCEPTIONS
TEST(Assign_Copy, ValuelessByException) {
  mpark::variant<int, move_thrower_t> v(42);
  EXPECT_THROW(v = move_thrower_t{}, MoveConstruction);
  EXPECT_TRUE(v.valueless_by_exception());
  mpark::variant<int, move_thrower_t> w(42);
  w = v;
  EXPECT_TRUE(w.valueless_by_exception());
}
#endif
