// Copyright 2018 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/container/internal/container_memory.h"

#include <cstdint>
#include <tuple>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/string_view.h"

namespace absl {
namespace container_internal {
namespace {

using ::testing::Pair;

TEST(Memory, AlignmentLargerThanBase) {
  std::allocator<int8_t> alloc;
  void* mem = Allocate<2>(&alloc, 3);
  EXPECT_EQ(0, reinterpret_cast<uintptr_t>(mem) % 2);
  memcpy(mem, "abc", 3);
  Deallocate<2>(&alloc, mem, 3);
}

TEST(Memory, AlignmentSmallerThanBase) {
  std::allocator<int64_t> alloc;
  void* mem = Allocate<2>(&alloc, 3);
  EXPECT_EQ(0, reinterpret_cast<uintptr_t>(mem) % 2);
  memcpy(mem, "abc", 3);
  Deallocate<2>(&alloc, mem, 3);
}

class Fixture : public ::testing::Test {
  using Alloc = std::allocator<std::string>;

 public:
  Fixture() { ptr_ = std::allocator_traits<Alloc>::allocate(*alloc(), 1); }
  ~Fixture() override {
    std::allocator_traits<Alloc>::destroy(*alloc(), ptr_);
    std::allocator_traits<Alloc>::deallocate(*alloc(), ptr_, 1);
  }
  std::string* ptr() { return ptr_; }
  Alloc* alloc() { return &alloc_; }

 private:
  Alloc alloc_;
  std::string* ptr_;
};

TEST_F(Fixture, ConstructNoArgs) {
  ConstructFromTuple(alloc(), ptr(), std::forward_as_tuple());
  EXPECT_EQ(*ptr(), "");
}

TEST_F(Fixture, ConstructOneArg) {
  ConstructFromTuple(alloc(), ptr(), std::forward_as_tuple("abcde"));
  EXPECT_EQ(*ptr(), "abcde");
}

TEST_F(Fixture, ConstructTwoArg) {
  ConstructFromTuple(alloc(), ptr(), std::forward_as_tuple(5, 'a'));
  EXPECT_EQ(*ptr(), "aaaaa");
}

TEST(PairArgs, NoArgs) {
  EXPECT_THAT(PairArgs(),
              Pair(std::forward_as_tuple(), std::forward_as_tuple()));
}

TEST(PairArgs, TwoArgs) {
  EXPECT_EQ(
      std::make_pair(std::forward_as_tuple(1), std::forward_as_tuple('A')),
      PairArgs(1, 'A'));
}

TEST(PairArgs, Pair) {
  EXPECT_EQ(
      std::make_pair(std::forward_as_tuple(1), std::forward_as_tuple('A')),
      PairArgs(std::make_pair(1, 'A')));
}

TEST(PairArgs, Piecewise) {
  EXPECT_EQ(
      std::make_pair(std::forward_as_tuple(1), std::forward_as_tuple('A')),
      PairArgs(std::piecewise_construct, std::forward_as_tuple(1),
               std::forward_as_tuple('A')));
}

TEST(WithConstructed, Simple) {
  EXPECT_EQ(1, WithConstructed<absl::string_view>(
                   std::make_tuple(std::string("a")),
                   [](absl::string_view str) { return str.size(); }));
}

template <class F, class Arg>
decltype(DecomposeValue(std::declval<F>(), std::declval<Arg>()))
DecomposeValueImpl(int, F&& f, Arg&& arg) {
  return DecomposeValue(std::forward<F>(f), std::forward<Arg>(arg));
}

template <class F, class Arg>
const char* DecomposeValueImpl(char, F&& f, Arg&& arg) {
  return "not decomposable";
}

template <class F, class Arg>
decltype(DecomposeValueImpl(0, std::declval<F>(), std::declval<Arg>()))
TryDecomposeValue(F&& f, Arg&& arg) {
  return DecomposeValueImpl(0, std::forward<F>(f), std::forward<Arg>(arg));
}

TEST(DecomposeValue, Decomposable) {
  auto f = [](const int& x, int&& y) {
    EXPECT_EQ(&x, &y);
    EXPECT_EQ(42, x);
    return 'A';
  };
  EXPECT_EQ('A', TryDecomposeValue(f, 42));
}

TEST(DecomposeValue, NotDecomposable) {
  auto f = [](void*) {
    ADD_FAILURE() << "Must not be called";
    return 'A';
  };
  EXPECT_STREQ("not decomposable", TryDecomposeValue(f, 42));
}

template <class F, class... Args>
decltype(DecomposePair(std::declval<F>(), std::declval<Args>()...))
DecomposePairImpl(int, F&& f, Args&&... args) {
  return DecomposePair(std::forward<F>(f), std::forward<Args>(args)...);
}

template <class F, class... Args>
const char* DecomposePairImpl(char, F&& f, Args&&... args) {
  return "not decomposable";
}

template <class F, class... Args>
decltype(DecomposePairImpl(0, std::declval<F>(), std::declval<Args>()...))
TryDecomposePair(F&& f, Args&&... args) {
  return DecomposePairImpl(0, std::forward<F>(f), std::forward<Args>(args)...);
}

TEST(DecomposePair, Decomposable) {
  auto f = [](const int& x, std::piecewise_construct_t, std::tuple<int&&> k,
              std::tuple<double>&& v) {
    EXPECT_EQ(&x, &std::get<0>(k));
    EXPECT_EQ(42, x);
    EXPECT_EQ(0.5, std::get<0>(v));
    return 'A';
  };
  EXPECT_EQ('A', TryDecomposePair(f, 42, 0.5));
  EXPECT_EQ('A', TryDecomposePair(f, std::make_pair(42, 0.5)));
  EXPECT_EQ('A', TryDecomposePair(f, std::piecewise_construct,
                                  std::make_tuple(42), std::make_tuple(0.5)));
}

TEST(DecomposePair, NotDecomposable) {
  auto f = [](...) {
    ADD_FAILURE() << "Must not be called";
    return 'A';
  };
  EXPECT_STREQ("not decomposable",
               TryDecomposePair(f));
  EXPECT_STREQ("not decomposable",
               TryDecomposePair(f, std::piecewise_construct, std::make_tuple(),
                                std::make_tuple(0.5)));
}

}  // namespace
}  // namespace container_internal
}  // namespace absl
