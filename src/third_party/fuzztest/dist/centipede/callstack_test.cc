// Copyright 2023 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/callstack.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/nullability.h"
#include "absl/container/flat_hash_set.h"
#include "./common/defs.h"

namespace fuzztest::internal {
namespace {

using ::testing::Pointwise;

// Simple test, calls OnFunctionEntry with fake sp values.
TEST(CallStack, SimpleTest) {
  static CallStack<> cs;  // CallStack should be global/tls only.
  cs.Reset(10);
  constexpr uintptr_t pc0 = 100;
  constexpr uintptr_t pc1 = 101;
  constexpr uintptr_t pc2 = 102;
  constexpr uintptr_t pc3 = 103;
  constexpr uintptr_t stack_top = 10000;
  EXPECT_EQ(cs.Depth(), 0);
  cs.OnFunctionEntry(pc0, stack_top);
  cs.OnFunctionEntry(pc1, stack_top - 1);
  cs.OnFunctionEntry(pc2, stack_top - 2);
  EXPECT_EQ(cs.Depth(), 3);
  EXPECT_EQ(cs.PC(0), pc0);
  EXPECT_EQ(cs.PC(1), pc1);
  EXPECT_EQ(cs.PC(2), pc2);
  cs.OnFunctionEntry(pc3, stack_top - 2);
  EXPECT_EQ(cs.Depth(), 3);
  EXPECT_EQ(cs.PC(2), pc3);
  cs.OnFunctionEntry(pc3, stack_top - 1);
  EXPECT_EQ(cs.Depth(), 2);
  EXPECT_EQ(cs.PC(1), pc3);
  cs.OnFunctionEntry(pc3, stack_top);
  EXPECT_EQ(cs.Depth(), 1);
  EXPECT_EQ(cs.PC(0), pc3);
}

static CallStack<> g_real_calls_cs;  // CallStack should be global/tls only.
using TestCallstack = std::vector<uintptr_t>;
static std::vector<TestCallstack> g_test_callstacks;

static void RecordCallStack() {
  TestCallstack test_callstack;
  for (size_t i = 0, n = g_real_calls_cs.Depth(); i < n; ++i) {
    test_callstack.push_back(g_real_calls_cs.PC(i));
  }
  g_test_callstacks.push_back(test_callstack);
}

// Call on entry to functions Func[123], that are helpers to RealCallsTest.
#define ON_ENTRY(PC)               \
  g_real_calls_cs.OnFunctionEntry( \
      PC, reinterpret_cast<uintptr_t>(__builtin_frame_address(0)))

// Don't let the compiler be too smart.
static inline void BreakOptimization(const void *absl_nullable arg) {
  __asm__ __volatile__("" : : "r"(arg) : "memory");
}

__attribute__((noinline)) void Func3() {
  ON_ENTRY(3);
  RecordCallStack();
  BreakOptimization(0);
}

__attribute__((noinline)) void Func2() {
  ON_ENTRY(2);
  BreakOptimization(0);
  Func3();
  BreakOptimization(0);
  Func3();
  BreakOptimization(0);
}

__attribute__((noinline)) void Func1() {
  ON_ENTRY(1);
  BreakOptimization(0);
  Func2();
  BreakOptimization(0);
  Func3();
  BreakOptimization(0);
}

__attribute__((noinline)) void Func0() {
  ON_ENTRY(0);
  BreakOptimization(0);
  Func1();
  BreakOptimization(0);
  Func2();
  BreakOptimization(0);
}

// A 2-tuple matcher conversion of `::testing::IsSupersetOf`.
MATCHER(IsSupersetOf, "") {
  auto [actual, expected] = arg;
  return ::testing::ExplainMatchResult(::testing::IsSupersetOf(expected),
                                       actual, result_listener);
}

// This test actually creates a function call tree, and calls OnFunctionEntry
// with real sp values (and fake PCs).
TEST(CallStack, RealCallsTest) {
  g_test_callstacks.clear();
  g_real_calls_cs.Reset(10);
  Func0();
  Func1();
  Func2();
  Func3();
  std::vector<TestCallstack> expected_test_callstacks = {
      {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 3}, {0, 2, 3}, {0, 2, 3}, {1, 2, 3},
      {1, 2, 3},    {1, 3},       {2, 3},    {2, 3},    {3}};

  // Each computed callstack should correctly include every function on the
  // callstack. It may also contain some additional spurious functions - these
  // are ones that have exited but not yet removed.
  EXPECT_THAT(g_test_callstacks,
              Pointwise(IsSupersetOf(), expected_test_callstacks));

  // Check that the additional elements in each computed callstack only
  // correspond to previous calls not yet removed.
  for (TestCallstack &cs : g_test_callstacks) {
    std::sort(cs.begin(), cs.end());
  }
  for (TestCallstack &cs : expected_test_callstacks) {
    std::sort(cs.begin(), cs.end());
  }
  std::vector<TestCallstack> extra_calls(g_test_callstacks.size());
  for (auto it_1 = g_test_callstacks.begin(),
            it_2 = expected_test_callstacks.begin(), it = extra_calls.begin();
       it_1 != g_test_callstacks.end(); it_1++, it_2++, it++) {
    std::set_difference(it_1->begin(), it_1->end(), it_2->begin(), it_2->end(),
                        std::inserter(*it, it->begin()));
  }
  EXPECT_THAT(std::vector<TestCallstack>(g_test_callstacks.begin(),
                                         g_test_callstacks.end() - 1),
              Pointwise(IsSupersetOf(),
                        std::vector<TestCallstack>(extra_calls.begin() + 1,
                                                   extra_calls.end())));
}

// Tests deep recursion.
TEST(CallStack, DeepRecursion) {
  static CallStack<100> cs;  // CallStack should be global/tls only.
  cs.Reset(10);
  constexpr size_t kLargeDepth = 200;
  constexpr uintptr_t kStackTop = 100000000;
  // Enter deep recursion.
  for (size_t i = 0; i < kLargeDepth; ++i) {
    cs.OnFunctionEntry(i, kStackTop - i);
  }
  EXPECT_EQ(cs.Depth(), 100);
  // Exit recursion, call not-too-deep.
  cs.OnFunctionEntry(42, kStackTop - 2);
  EXPECT_EQ(cs.Depth(), 3);
  EXPECT_EQ(cs.PC(0), 0);
  EXPECT_EQ(cs.PC(1), 1);
  EXPECT_EQ(cs.PC(2), 42);
}

// Tests CallStack::Hash().
TEST(CallStack, Hash) {
  constexpr size_t kDepth = 5000;
  constexpr size_t kNumDifferentPCs = 10000;
  constexpr size_t kNumIterations = 1000;
  constexpr uintptr_t kStackTop = 100000000;
  static CallStack<kDepth> cs;  // CallStack should be global/tls only.
  cs.Reset(10);
  fuzztest::internal::Rng rng;

  // Push the first PC on the stack, remembers it hash.
  cs.OnFunctionEntry(42, kStackTop);
  const auto initial_hash = cs.Hash();

  absl::flat_hash_set<uintptr_t> hashes;

  for (size_t iter = 0; iter < kNumIterations; ++iter) {
    // Push many PCs on the stack, collect their hashes.
    hashes.clear();
    for (size_t i = 0; i < kDepth; ++i) {
      cs.OnFunctionEntry(rng() % kNumDifferentPCs, kStackTop - i);
      auto hash = cs.Hash();
      hashes.insert(hash);
    }
    // Check that most hashes are unique. Some collisions are ok.
    EXPECT_GE(hashes.size(), kDepth - 1);
    // unwind all the way to the top.
    cs.OnFunctionEntry(42, kStackTop);
    EXPECT_EQ(cs.Depth(), 1);
    EXPECT_EQ(cs.Hash(), initial_hash);
  }
}

TEST(CallStack, WindowSize) {
  constexpr size_t kDepth = 5000;
  constexpr uintptr_t kStackTop = 100000000;
  static CallStack<kDepth> cs;  // CallStack should be global/tls only.
  absl::flat_hash_set<uintptr_t> hashes;
  for (size_t num_different_frames = 1; num_different_frames < 100;
       ++num_different_frames) {
    for (size_t window_size = 1; window_size < 100; ++window_size) {
      // Simulate recursive call stack with `num_different_frames` period,
      // i.e. for `num_different_frames=3`, the call stack is
      // {42, 43, 44, 42, 43, 44, 42 ...}
      // Ensure that the hash() function respects the window size.
      hashes.clear();
      cs.Reset(window_size);
      cs.OnFunctionEntry(42, kStackTop);
      for (size_t i = 0; i < kDepth; ++i) {
        cs.OnFunctionEntry(42 + (i % num_different_frames), kStackTop - i);
        hashes.insert(cs.Hash());
      }
      EXPECT_EQ(hashes.size(), window_size + num_different_frames - 1);
    }
  }
}

}  // namespace
}  // namespace fuzztest::internal
