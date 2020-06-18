// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#include <mpark/variant.hpp>

#include <string>

#include <gtest/gtest.h>

#include "util.hpp"

TEST(Swap, Same) {
  mpark::variant<int, std::string> v("hello");
  mpark::variant<int, std::string> w("world");
  // Check `v`.
  EXPECT_EQ("hello", mpark::get<std::string>(v));
  // Check `w`.
  EXPECT_EQ("world", mpark::get<std::string>(w));
  // Swap.
  using std::swap;
  swap(v, w);
  // Check `v`.
  EXPECT_EQ("world", mpark::get<std::string>(v));
  // Check `w`.
  EXPECT_EQ("hello", mpark::get<std::string>(w));
}

TEST(Swap, Different) {
  mpark::variant<int, std::string> v(42);
  mpark::variant<int, std::string> w("hello");
  // Check `v`.
  EXPECT_EQ(42, mpark::get<int>(v));
  // Check `w`.
  EXPECT_EQ("hello", mpark::get<std::string>(w));
  // Swap.
  using std::swap;
  swap(v, w);
  // Check `v`.
  EXPECT_EQ("hello", mpark::get<std::string>(v));
  // Check `w`.
  EXPECT_EQ(42, mpark::get<int>(w));
}

#ifdef MPARK_EXCEPTIONS
TEST(Swap, OneValuelessByException) {
  // `v` normal, `w` corrupted.
  mpark::variant<int, move_thrower_t> v(42), w(42);
  EXPECT_THROW(w = move_thrower_t{}, MoveConstruction);
  EXPECT_EQ(42, mpark::get<int>(v));
  EXPECT_TRUE(w.valueless_by_exception());
  // Swap.
  using std::swap;
  swap(v, w);
  // Check `v`, `w`.
  EXPECT_TRUE(v.valueless_by_exception());
  EXPECT_EQ(42, mpark::get<int>(w));
}

TEST(Swap, BothValuelessByException) {
  // `v`, `w` both corrupted.
  mpark::variant<int, move_thrower_t> v(42);
  EXPECT_THROW(v = move_thrower_t{}, MoveConstruction);
  mpark::variant<int, move_thrower_t> w(v);
  EXPECT_TRUE(v.valueless_by_exception());
  EXPECT_TRUE(w.valueless_by_exception());
  // Swap.
  using std::swap;
  swap(v, w);
  // Check `v`, `w`.
  EXPECT_TRUE(v.valueless_by_exception());
  EXPECT_TRUE(w.valueless_by_exception());
}
#endif

TEST(Swap, DtorsSame) {
  struct Obj {
    Obj(size_t *dtor_count) : dtor_count_(dtor_count) {}
    Obj(const Obj &) = default;
    Obj(Obj &&) = default;
    ~Obj() { ++(*dtor_count_); }
    Obj &operator=(const Obj &) = default;
    Obj &operator=(Obj &&) = default;
    size_t *dtor_count_;
  };  // Obj
  size_t v_count = 0;
  size_t w_count = 0;
  {
    mpark::variant<Obj> v{&v_count}, w{&w_count};
    using std::swap;
    swap(v, w);
    // Calls `std::swap(Obj &lhs, Obj &rhs)`, with which we perform:
    // ```
    // {
    //   Obj temp(move(lhs));
    //   lhs = move(rhs);
    //   rhs = move(temp);
    // }  `++v_count` from `temp::~Obj()`.
    // ```
    EXPECT_EQ(1u, v_count);
    EXPECT_EQ(0u, w_count);
  }
  EXPECT_EQ(2u, v_count);
  EXPECT_EQ(1u, w_count);
}

namespace detail {

struct Obj {
  Obj(size_t *dtor_count) : dtor_count_(dtor_count) {}
  Obj(const Obj &) = default;
  Obj(Obj &&) = default;
  ~Obj() { ++(*dtor_count_); }
  Obj &operator=(const Obj &) = default;
  Obj &operator=(Obj &&) = default;
  size_t *dtor_count_;
};  // Obj

static void swap(Obj &lhs, Obj &rhs) noexcept {
  std::swap(lhs.dtor_count_, rhs.dtor_count_);
}

}  // namespace detail

TEST(Swap, DtorsSameWithSwap) {
  size_t v_count = 0;
  size_t w_count = 0;
  {
    mpark::variant<detail::Obj> v{&v_count}, w{&w_count};
    using std::swap;
    swap(v, w);
    // Calls `detail::swap(Obj &lhs, Obj &rhs)`, with which doesn't call any destructors.
    EXPECT_EQ(0u, v_count);
    EXPECT_EQ(0u, w_count);
  }
  EXPECT_EQ(1u, v_count);
  EXPECT_EQ(1u, w_count);
}

TEST(Swap, DtorsDifferent) {
  struct V {
    V(size_t *dtor_count) : dtor_count_(dtor_count) {}
    V(const V &) = default;
    V(V &&) = default;
    ~V() { ++(*dtor_count_); }
    V &operator=(const V &) = default;
    V &operator=(V &&) = default;
    size_t *dtor_count_;
  };  // V
  struct W {
    W(size_t *dtor_count) : dtor_count_(dtor_count) {}
    W(const W &) = default;
    W(W &&) = default;
    ~W() { ++(*dtor_count_); }
    W &operator=(const W &) = default;
    W &operator=(W &&) = default;
    size_t *dtor_count_;
  };  // W
  size_t v_count = 0;
  size_t w_count = 0;
  {
    mpark::variant<V, W> v{mpark::in_place_type_t<V>{}, &v_count};
    mpark::variant<V, W> w{mpark::in_place_type_t<W>{}, &w_count};
    using std::swap;
    swap(v, w);
    EXPECT_EQ(1u, v_count);
    EXPECT_EQ(2u, w_count);
  }
  EXPECT_EQ(2u, v_count);
  EXPECT_EQ(3u, w_count);
}
