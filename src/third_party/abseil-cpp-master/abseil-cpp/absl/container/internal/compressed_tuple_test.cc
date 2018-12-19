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

#include "absl/container/internal/compressed_tuple.h"

#include <memory>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/memory/memory.h"
#include "absl/utility/utility.h"

namespace absl {
namespace container_internal {
namespace {

enum class CallType { kConstRef, kConstMove };

template <int>
struct Empty {
  constexpr CallType value() const& { return CallType::kConstRef; }
  constexpr CallType value() const&& { return CallType::kConstMove; }
};

template <typename T>
struct NotEmpty {
  T value;
};

template <typename T, typename U>
struct TwoValues {
  T value1;
  U value2;
};

TEST(CompressedTupleTest, Sizeof) {
  EXPECT_EQ(sizeof(int), sizeof(CompressedTuple<int>));
  EXPECT_EQ(sizeof(int), sizeof(CompressedTuple<int, Empty<0>>));
  EXPECT_EQ(sizeof(int), sizeof(CompressedTuple<int, Empty<0>, Empty<1>>));
  EXPECT_EQ(sizeof(int),
            sizeof(CompressedTuple<int, Empty<0>, Empty<1>, Empty<2>>));

  EXPECT_EQ(sizeof(TwoValues<int, double>),
            sizeof(CompressedTuple<int, NotEmpty<double>>));
  EXPECT_EQ(sizeof(TwoValues<int, double>),
            sizeof(CompressedTuple<int, Empty<0>, NotEmpty<double>>));
  EXPECT_EQ(sizeof(TwoValues<int, double>),
            sizeof(CompressedTuple<int, Empty<0>, NotEmpty<double>, Empty<1>>));
}

TEST(CompressedTupleTest, Access) {
  struct S {
    std::string x;
  };
  CompressedTuple<int, Empty<0>, S> x(7, {}, S{"ABC"});
  EXPECT_EQ(sizeof(x), sizeof(TwoValues<int, S>));
  EXPECT_EQ(7, x.get<0>());
  EXPECT_EQ("ABC", x.get<2>().x);
}

TEST(CompressedTupleTest, NonClasses) {
  CompressedTuple<int, const char*> x(7, "ABC");
  EXPECT_EQ(7, x.get<0>());
  EXPECT_STREQ("ABC", x.get<1>());
}

TEST(CompressedTupleTest, MixClassAndNonClass) {
  CompressedTuple<int, const char*, Empty<0>, NotEmpty<double>> x(7, "ABC", {},
                                                                  {1.25});
  struct Mock {
    int v;
    const char* p;
    double d;
  };
  EXPECT_EQ(sizeof(x), sizeof(Mock));
  EXPECT_EQ(7, x.get<0>());
  EXPECT_STREQ("ABC", x.get<1>());
  EXPECT_EQ(1.25, x.get<3>().value);
}

TEST(CompressedTupleTest, Nested) {
  CompressedTuple<int, CompressedTuple<int>,
                  CompressedTuple<int, CompressedTuple<int>>>
      x(1, CompressedTuple<int>(2),
        CompressedTuple<int, CompressedTuple<int>>(3, CompressedTuple<int>(4)));
  EXPECT_EQ(1, x.get<0>());
  EXPECT_EQ(2, x.get<1>().get<0>());
  EXPECT_EQ(3, x.get<2>().get<0>());
  EXPECT_EQ(4, x.get<2>().get<1>().get<0>());

  CompressedTuple<Empty<0>, Empty<0>,
                  CompressedTuple<Empty<0>, CompressedTuple<Empty<0>>>>
      y;
  std::set<Empty<0>*> empties{&y.get<0>(), &y.get<1>(), &y.get<2>().get<0>(),
                              &y.get<2>().get<1>().get<0>()};
#ifdef _MSC_VER
  // MSVC has a bug where many instances of the same base class are layed out in
  // the same address when using __declspec(empty_bases).
  // This will be fixed in a future version of MSVC.
  int expected = 1;
#else
  int expected = 4;
#endif
  EXPECT_EQ(expected, sizeof(y));
  EXPECT_EQ(expected, empties.size());
  EXPECT_EQ(sizeof(y), sizeof(Empty<0>) * empties.size());

  EXPECT_EQ(4 * sizeof(char),
            sizeof(CompressedTuple<CompressedTuple<char, char>,
                                   CompressedTuple<char, char>>));
  EXPECT_TRUE(
      (std::is_empty<CompressedTuple<CompressedTuple<Empty<0>>,
                                     CompressedTuple<Empty<1>>>>::value));
}

TEST(CompressedTupleTest, Reference) {
  int i = 7;
  std::string s = "Very long std::string that goes in the heap";
  CompressedTuple<int, int&, std::string, std::string&> x(i, i, s, s);

  // Sanity check. We should have not moved from `s`
  EXPECT_EQ(s, "Very long std::string that goes in the heap");

  EXPECT_EQ(x.get<0>(), x.get<1>());
  EXPECT_NE(&x.get<0>(), &x.get<1>());
  EXPECT_EQ(&x.get<1>(), &i);

  EXPECT_EQ(x.get<2>(), x.get<3>());
  EXPECT_NE(&x.get<2>(), &x.get<3>());
  EXPECT_EQ(&x.get<3>(), &s);
}

TEST(CompressedTupleTest, NoElements) {
  CompressedTuple<> x;
  static_cast<void>(x);  // Silence -Wunused-variable.
  EXPECT_TRUE(std::is_empty<CompressedTuple<>>::value);
}

TEST(CompressedTupleTest, MoveOnlyElements) {
  CompressedTuple<std::unique_ptr<std::string>> str_tup(
      absl::make_unique<std::string>("str"));

  CompressedTuple<CompressedTuple<std::unique_ptr<std::string>>,
                  std::unique_ptr<int>>
  x(std::move(str_tup), absl::make_unique<int>(5));

  EXPECT_EQ(*x.get<0>().get<0>(), "str");
  EXPECT_EQ(*x.get<1>(), 5);

  std::unique_ptr<std::string> x0 = std::move(x.get<0>()).get<0>();
  std::unique_ptr<int> x1 = std::move(x).get<1>();

  EXPECT_EQ(*x0, "str");
  EXPECT_EQ(*x1, 5);
}

TEST(CompressedTupleTest, Constexpr) {
  constexpr CompressedTuple<int, double, CompressedTuple<int>, Empty<0>> x(
      7, 1.25, CompressedTuple<int>(5), {});
  constexpr int x0 = x.get<0>();
  constexpr double x1 = x.get<1>();
  constexpr int x2 = x.get<2>().get<0>();
  constexpr CallType x3 = x.get<3>().value();

  EXPECT_EQ(x0, 7);
  EXPECT_EQ(x1, 1.25);
  EXPECT_EQ(x2, 5);
  EXPECT_EQ(x3, CallType::kConstRef);

#if defined(__clang__)
  // An apparent bug in earlier versions of gcc claims these are ambiguous.
  constexpr int x2m = absl::move(x.get<2>()).get<0>();
  constexpr CallType x3m = absl::move(x).get<3>().value();
  EXPECT_EQ(x2m, 5);
  EXPECT_EQ(x3m, CallType::kConstMove);
#endif
}

#if defined(__clang__) || defined(__GNUC__)
TEST(CompressedTupleTest, EmptyFinalClass) {
  struct S final {
    int f() const { return 5; }
  };
  CompressedTuple<S> x;
  EXPECT_EQ(x.get<0>().f(), 5);
}
#endif

}  // namespace
}  // namespace container_internal
}  // namespace absl
