// Copyright 2017 The Abseil Authors.
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

#include "absl/base/internal/invoke.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"

namespace absl {
namespace base_internal {
namespace {

int Function(int a, int b) { return a - b; }

int Sink(std::unique_ptr<int> p) {
  return *p;
}

std::unique_ptr<int> Factory(int n) {
  return make_unique<int>(n);
}

void NoOp() {}

struct ConstFunctor {
  int operator()(int a, int b) const { return a - b; }
};

struct MutableFunctor {
  int operator()(int a, int b) { return a - b; }
};

struct EphemeralFunctor {
  int operator()(int a, int b) && { return a - b; }
};

struct OverloadedFunctor {
  template <typename... Args>
  std::string operator()(const Args&... args) & {
    return StrCat("&", args...);
  }
  template <typename... Args>
  std::string operator()(const Args&... args) const& {
    return StrCat("const&", args...);
  }
  template <typename... Args>
  std::string operator()(const Args&... args) && {
    return StrCat("&&", args...);
  }
};

struct Class {
  int Method(int a, int b) { return a - b; }
  int ConstMethod(int a, int b) const { return a - b; }

  int member;
};

struct FlipFlop {
  int ConstMethod() const { return member; }
  FlipFlop operator*() const { return {-member}; }

  int member;
};

// CallMaybeWithArg(f) resolves either to Invoke(f) or Invoke(f, 42), depending
// on which one is valid.
template <typename F>
decltype(Invoke(std::declval<const F&>())) CallMaybeWithArg(const F& f) {
  return Invoke(f);
}

template <typename F>
decltype(Invoke(std::declval<const F&>(), 42)) CallMaybeWithArg(const F& f) {
  return Invoke(f, 42);
}

TEST(InvokeTest, Function) {
  EXPECT_EQ(1, Invoke(Function, 3, 2));
  EXPECT_EQ(1, Invoke(&Function, 3, 2));
}

TEST(InvokeTest, NonCopyableArgument) {
  EXPECT_EQ(42, Invoke(Sink, make_unique<int>(42)));
}

TEST(InvokeTest, NonCopyableResult) {
  EXPECT_THAT(Invoke(Factory, 42), ::testing::Pointee(42));
}

TEST(InvokeTest, VoidResult) {
  Invoke(NoOp);
}

TEST(InvokeTest, ConstFunctor) {
  EXPECT_EQ(1, Invoke(ConstFunctor(), 3, 2));
}

TEST(InvokeTest, MutableFunctor) {
  MutableFunctor f;
  EXPECT_EQ(1, Invoke(f, 3, 2));
  EXPECT_EQ(1, Invoke(MutableFunctor(), 3, 2));
}

TEST(InvokeTest, EphemeralFunctor) {
  EphemeralFunctor f;
  EXPECT_EQ(1, Invoke(std::move(f), 3, 2));
  EXPECT_EQ(1, Invoke(EphemeralFunctor(), 3, 2));
}

TEST(InvokeTest, OverloadedFunctor) {
  OverloadedFunctor f;
  const OverloadedFunctor& cf = f;

  EXPECT_EQ("&", Invoke(f));
  EXPECT_EQ("& 42", Invoke(f, " 42"));

  EXPECT_EQ("const&", Invoke(cf));
  EXPECT_EQ("const& 42", Invoke(cf, " 42"));

  EXPECT_EQ("&&", Invoke(std::move(f)));
  EXPECT_EQ("&& 42", Invoke(std::move(f), " 42"));
}

TEST(InvokeTest, ReferenceWrapper) {
  ConstFunctor cf;
  MutableFunctor mf;
  EXPECT_EQ(1, Invoke(std::cref(cf), 3, 2));
  EXPECT_EQ(1, Invoke(std::ref(cf), 3, 2));
  EXPECT_EQ(1, Invoke(std::ref(mf), 3, 2));
}

TEST(InvokeTest, MemberFunction) {
  std::unique_ptr<Class> p(new Class);
  std::unique_ptr<const Class> cp(new Class);
  EXPECT_EQ(1, Invoke(&Class::Method, p, 3, 2));
  EXPECT_EQ(1, Invoke(&Class::Method, p.get(), 3, 2));

  EXPECT_EQ(1, Invoke(&Class::ConstMethod, p, 3, 2));
  EXPECT_EQ(1, Invoke(&Class::ConstMethod, p.get(), 3, 2));
  EXPECT_EQ(1, Invoke(&Class::ConstMethod, *p, 3, 2));

  EXPECT_EQ(1, Invoke(&Class::ConstMethod, cp, 3, 2));
  EXPECT_EQ(1, Invoke(&Class::ConstMethod, cp.get(), 3, 2));
  EXPECT_EQ(1, Invoke(&Class::ConstMethod, *cp, 3, 2));

  EXPECT_EQ(1, Invoke(&Class::Method, make_unique<Class>(), 3, 2));
  EXPECT_EQ(1, Invoke(&Class::ConstMethod, make_unique<Class>(), 3, 2));
  EXPECT_EQ(1, Invoke(&Class::ConstMethod, make_unique<const Class>(), 3, 2));
}

TEST(InvokeTest, DataMember) {
  std::unique_ptr<Class> p(new Class{42});
  std::unique_ptr<const Class> cp(new Class{42});
  EXPECT_EQ(42, Invoke(&Class::member, p));
  EXPECT_EQ(42, Invoke(&Class::member, *p));
  EXPECT_EQ(42, Invoke(&Class::member, p.get()));

  Invoke(&Class::member, p) = 42;
  Invoke(&Class::member, p.get()) = 42;

  EXPECT_EQ(42, Invoke(&Class::member, cp));
  EXPECT_EQ(42, Invoke(&Class::member, *cp));
  EXPECT_EQ(42, Invoke(&Class::member, cp.get()));
}

TEST(InvokeTest, FlipFlop) {
  FlipFlop obj = {42};
  // This call could resolve to (obj.*&FlipFlop::ConstMethod)() or
  // ((*obj).*&FlipFlop::ConstMethod)(). We verify that it's the former.
  EXPECT_EQ(42, Invoke(&FlipFlop::ConstMethod, obj));
  EXPECT_EQ(42, Invoke(&FlipFlop::member, obj));
}

TEST(InvokeTest, SfinaeFriendly) {
  CallMaybeWithArg(NoOp);
  EXPECT_THAT(CallMaybeWithArg(Factory), ::testing::Pointee(42));
}

}  // namespace
}  // namespace base_internal
}  // namespace absl
