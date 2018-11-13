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

#include "absl/container/internal/raw_hash_set.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <numeric>
#include <random>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/container/internal/container_memory.h"
#include "absl/container/internal/hash_function_defaults.h"
#include "absl/container/internal/hash_policy_testing.h"
#include "absl/container/internal/hashtable_debug.h"
#include "absl/strings/string_view.h"

namespace absl {
namespace container_internal {

struct RawHashSetTestOnlyAccess {
  template <typename C>
  static auto GetSlots(const C& c) -> decltype(c.slots_) {
    return c.slots_;
  }
};

namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::Optional;
using ::testing::Pair;
using ::testing::UnorderedElementsAre;

TEST(Util, NormalizeCapacity) {
  constexpr size_t kMinCapacity = Group::kWidth - 1;
  EXPECT_EQ(kMinCapacity, NormalizeCapacity(0));
  EXPECT_EQ(kMinCapacity, NormalizeCapacity(1));
  EXPECT_EQ(kMinCapacity, NormalizeCapacity(2));
  EXPECT_EQ(kMinCapacity, NormalizeCapacity(kMinCapacity));
  EXPECT_EQ(kMinCapacity * 2 + 1, NormalizeCapacity(kMinCapacity + 1));
  EXPECT_EQ(kMinCapacity * 2 + 1, NormalizeCapacity(kMinCapacity + 2));
}

TEST(Util, probe_seq) {
  probe_seq<16> seq(0, 127);
  auto gen = [&]() {
    size_t res = seq.offset();
    seq.next();
    return res;
  };
  std::vector<size_t> offsets(8);
  std::generate_n(offsets.begin(), 8, gen);
  EXPECT_THAT(offsets, ElementsAre(0, 16, 48, 96, 32, 112, 80, 64));
  seq = probe_seq<16>(128, 127);
  std::generate_n(offsets.begin(), 8, gen);
  EXPECT_THAT(offsets, ElementsAre(0, 16, 48, 96, 32, 112, 80, 64));
}

TEST(BitMask, Smoke) {
  EXPECT_FALSE((BitMask<uint8_t, 8>(0)));
  EXPECT_TRUE((BitMask<uint8_t, 8>(5)));

  EXPECT_THAT((BitMask<uint8_t, 8>(0)), ElementsAre());
  EXPECT_THAT((BitMask<uint8_t, 8>(0x1)), ElementsAre(0));
  EXPECT_THAT((BitMask<uint8_t, 8>(0x2)), ElementsAre(1));
  EXPECT_THAT((BitMask<uint8_t, 8>(0x3)), ElementsAre(0, 1));
  EXPECT_THAT((BitMask<uint8_t, 8>(0x4)), ElementsAre(2));
  EXPECT_THAT((BitMask<uint8_t, 8>(0x5)), ElementsAre(0, 2));
  EXPECT_THAT((BitMask<uint8_t, 8>(0x55)), ElementsAre(0, 2, 4, 6));
  EXPECT_THAT((BitMask<uint8_t, 8>(0xAA)), ElementsAre(1, 3, 5, 7));
}

TEST(BitMask, WithShift) {
  // See the non-SSE version of Group for details on what this math is for.
  uint64_t ctrl = 0x1716151413121110;
  uint64_t hash = 0x12;
  constexpr uint64_t msbs = 0x8080808080808080ULL;
  constexpr uint64_t lsbs = 0x0101010101010101ULL;
  auto x = ctrl ^ (lsbs * hash);
  uint64_t mask = (x - lsbs) & ~x & msbs;
  EXPECT_EQ(0x0000000080800000, mask);

  BitMask<uint64_t, 8, 3> b(mask);
  EXPECT_EQ(*b, 2);
}

TEST(BitMask, LeadingTrailing) {
  EXPECT_EQ((BitMask<uint32_t, 16>(0b0001101001000000).LeadingZeros()), 3);
  EXPECT_EQ((BitMask<uint32_t, 16>(0b0001101001000000).TrailingZeros()), 6);

  EXPECT_EQ((BitMask<uint32_t, 16>(0b0000000000000001).LeadingZeros()), 15);
  EXPECT_EQ((BitMask<uint32_t, 16>(0b0000000000000001).TrailingZeros()), 0);

  EXPECT_EQ((BitMask<uint32_t, 16>(0b1000000000000000).LeadingZeros()), 0);
  EXPECT_EQ((BitMask<uint32_t, 16>(0b1000000000000000).TrailingZeros()), 15);

  EXPECT_EQ((BitMask<uint64_t, 8, 3>(0x0000008080808000).LeadingZeros()), 3);
  EXPECT_EQ((BitMask<uint64_t, 8, 3>(0x0000008080808000).TrailingZeros()), 1);

  EXPECT_EQ((BitMask<uint64_t, 8, 3>(0x0000000000000080).LeadingZeros()), 7);
  EXPECT_EQ((BitMask<uint64_t, 8, 3>(0x0000000000000080).TrailingZeros()), 0);

  EXPECT_EQ((BitMask<uint64_t, 8, 3>(0x8000000000000000).LeadingZeros()), 0);
  EXPECT_EQ((BitMask<uint64_t, 8, 3>(0x8000000000000000).TrailingZeros()), 7);
}

TEST(Group, EmptyGroup) {
  for (h2_t h = 0; h != 128; ++h) EXPECT_FALSE(Group{EmptyGroup()}.Match(h));
}

TEST(Group, Match) {
  if (Group::kWidth == 16) {
    ctrl_t group[] = {kEmpty, 1, kDeleted, 3, kEmpty, 5, kSentinel, 7,
                      7,      5, 3,        1, 1,      1, 1,         1};
    EXPECT_THAT(Group{group}.Match(0), ElementsAre());
    EXPECT_THAT(Group{group}.Match(1), ElementsAre(1, 11, 12, 13, 14, 15));
    EXPECT_THAT(Group{group}.Match(3), ElementsAre(3, 10));
    EXPECT_THAT(Group{group}.Match(5), ElementsAre(5, 9));
    EXPECT_THAT(Group{group}.Match(7), ElementsAre(7, 8));
  } else if (Group::kWidth == 8) {
    ctrl_t group[] = {kEmpty, 1, 2, kDeleted, 2, 1, kSentinel, 1};
    EXPECT_THAT(Group{group}.Match(0), ElementsAre());
    EXPECT_THAT(Group{group}.Match(1), ElementsAre(1, 5, 7));
    EXPECT_THAT(Group{group}.Match(2), ElementsAre(2, 4));
  } else {
    FAIL() << "No test coverage for Group::kWidth==" << Group::kWidth;
  }
}

TEST(Group, MatchEmpty) {
  if (Group::kWidth == 16) {
    ctrl_t group[] = {kEmpty, 1, kDeleted, 3, kEmpty, 5, kSentinel, 7,
                      7,      5, 3,        1, 1,      1, 1,         1};
    EXPECT_THAT(Group{group}.MatchEmpty(), ElementsAre(0, 4));
  } else if (Group::kWidth == 8) {
    ctrl_t group[] = {kEmpty, 1, 2, kDeleted, 2, 1, kSentinel, 1};
    EXPECT_THAT(Group{group}.MatchEmpty(), ElementsAre(0));
  } else {
    FAIL() << "No test coverage for Group::kWidth==" << Group::kWidth;
  }
}

TEST(Group, MatchEmptyOrDeleted) {
  if (Group::kWidth == 16) {
    ctrl_t group[] = {kEmpty, 1, kDeleted, 3, kEmpty, 5, kSentinel, 7,
                      7,      5, 3,        1, 1,      1, 1,         1};
    EXPECT_THAT(Group{group}.MatchEmptyOrDeleted(), ElementsAre(0, 2, 4));
  } else if (Group::kWidth == 8) {
    ctrl_t group[] = {kEmpty, 1, 2, kDeleted, 2, 1, kSentinel, 1};
    EXPECT_THAT(Group{group}.MatchEmptyOrDeleted(), ElementsAre(0, 3));
  } else {
    FAIL() << "No test coverage for Group::kWidth==" << Group::kWidth;
  }
}

TEST(Batch, DropDeletes) {
  constexpr size_t kCapacity = 63;
  constexpr size_t kGroupWidth = container_internal::Group::kWidth;
  std::vector<ctrl_t> ctrl(kCapacity + 1 + kGroupWidth);
  ctrl[kCapacity] = kSentinel;
  std::vector<ctrl_t> pattern = {kEmpty, 2, kDeleted, 2, kEmpty, 1, kDeleted};
  for (size_t i = 0; i != kCapacity; ++i) {
    ctrl[i] = pattern[i % pattern.size()];
    if (i < kGroupWidth - 1)
      ctrl[i + kCapacity + 1] = pattern[i % pattern.size()];
  }
  ConvertDeletedToEmptyAndFullToDeleted(ctrl.data(), kCapacity);
  ASSERT_EQ(ctrl[kCapacity], kSentinel);
  for (size_t i = 0; i < kCapacity + 1 + kGroupWidth; ++i) {
    ctrl_t expected = pattern[i % (kCapacity + 1) % pattern.size()];
    if (i == kCapacity) expected = kSentinel;
    if (expected == kDeleted) expected = kEmpty;
    if (IsFull(expected)) expected = kDeleted;
    EXPECT_EQ(ctrl[i], expected)
        << i << " " << int{pattern[i % pattern.size()]};
  }
}

TEST(Group, CountLeadingEmptyOrDeleted) {
  const std::vector<ctrl_t> empty_examples = {kEmpty, kDeleted};
  const std::vector<ctrl_t> full_examples = {0, 1, 2, 3, 5, 9, 127, kSentinel};

  for (ctrl_t empty : empty_examples) {
    std::vector<ctrl_t> e(Group::kWidth, empty);
    EXPECT_EQ(Group::kWidth, Group{e.data()}.CountLeadingEmptyOrDeleted());
    for (ctrl_t full : full_examples) {
      for (size_t i = 0; i != Group::kWidth; ++i) {
        std::vector<ctrl_t> f(Group::kWidth, empty);
        f[i] = full;
        EXPECT_EQ(i, Group{f.data()}.CountLeadingEmptyOrDeleted());
      }
      std::vector<ctrl_t> f(Group::kWidth, empty);
      f[Group::kWidth * 2 / 3] = full;
      f[Group::kWidth / 2] = full;
      EXPECT_EQ(
          Group::kWidth / 2, Group{f.data()}.CountLeadingEmptyOrDeleted());
    }
  }
}

struct IntPolicy {
  using slot_type = int64_t;
  using key_type = int64_t;
  using init_type = int64_t;

  static void construct(void*, int64_t* slot, int64_t v) { *slot = v; }
  static void destroy(void*, int64_t*) {}
  static void transfer(void*, int64_t* new_slot, int64_t* old_slot) {
    *new_slot = *old_slot;
  }

  static int64_t& element(slot_type* slot) { return *slot; }

  template <class F>
  static auto apply(F&& f, int64_t x) -> decltype(std::forward<F>(f)(x, x)) {
    return std::forward<F>(f)(x, x);
  }
};

class StringPolicy {
  template <class F, class K, class V,
            class = typename std::enable_if<
                std::is_convertible<const K&, absl::string_view>::value>::type>
  decltype(std::declval<F>()(
      std::declval<const absl::string_view&>(), std::piecewise_construct,
      std::declval<std::tuple<K>>(),
      std::declval<V>())) static apply_impl(F&& f,
                                            std::pair<std::tuple<K>, V> p) {
    const absl::string_view& key = std::get<0>(p.first);
    return std::forward<F>(f)(key, std::piecewise_construct, std::move(p.first),
                              std::move(p.second));
  }

 public:
  struct slot_type {
    struct ctor {};

    template <class... Ts>
    slot_type(ctor, Ts&&... ts) : pair(std::forward<Ts>(ts)...) {}

    std::pair<std::string, std::string> pair;
  };

  using key_type = std::string;
  using init_type = std::pair<std::string, std::string>;

  template <class allocator_type, class... Args>
  static void construct(allocator_type* alloc, slot_type* slot, Args... args) {
    std::allocator_traits<allocator_type>::construct(
        *alloc, slot, typename slot_type::ctor(), std::forward<Args>(args)...);
  }

  template <class allocator_type>
  static void destroy(allocator_type* alloc, slot_type* slot) {
    std::allocator_traits<allocator_type>::destroy(*alloc, slot);
  }

  template <class allocator_type>
  static void transfer(allocator_type* alloc, slot_type* new_slot,
                       slot_type* old_slot) {
    construct(alloc, new_slot, std::move(old_slot->pair));
    destroy(alloc, old_slot);
  }

  static std::pair<std::string, std::string>& element(slot_type* slot) {
    return slot->pair;
  }

  template <class F, class... Args>
  static auto apply(F&& f, Args&&... args)
      -> decltype(apply_impl(std::forward<F>(f),
                             PairArgs(std::forward<Args>(args)...))) {
    return apply_impl(std::forward<F>(f),
                      PairArgs(std::forward<Args>(args)...));
  }
};

struct StringHash : absl::Hash<absl::string_view> {
  using is_transparent = void;
};
struct StringEq : std::equal_to<absl::string_view> {
  using is_transparent = void;
};

struct StringTable
    : raw_hash_set<StringPolicy, StringHash, StringEq, std::allocator<int>> {
  using Base = typename StringTable::raw_hash_set;
  StringTable() {}
  using Base::Base;
};

struct IntTable
    : raw_hash_set<IntPolicy, container_internal::hash_default_hash<int64_t>,
                   std::equal_to<int64_t>, std::allocator<int64_t>> {
  using Base = typename IntTable::raw_hash_set;
  IntTable() {}
  using Base::Base;
};

struct BadFastHash {
  template <class T>
  size_t operator()(const T&) const {
    return 0;
  }
};

struct BadTable : raw_hash_set<IntPolicy, BadFastHash, std::equal_to<int>,
                               std::allocator<int>> {
  using Base = typename BadTable::raw_hash_set;
  BadTable() {}
  using Base::Base;
};

TEST(Table, EmptyFunctorOptimization) {
  static_assert(std::is_empty<std::equal_to<absl::string_view>>::value, "");
  static_assert(std::is_empty<std::allocator<int>>::value, "");

  struct MockTable {
    void* ctrl;
    void* slots;
    size_t size;
    size_t capacity;
    size_t growth_left;
  };
  struct StatelessHash {
    size_t operator()(absl::string_view) const { return 0; }
  };
  struct StatefulHash : StatelessHash {
    size_t dummy;
  };

  EXPECT_EQ(
      sizeof(MockTable),
      sizeof(
          raw_hash_set<StringPolicy, StatelessHash,
                       std::equal_to<absl::string_view>, std::allocator<int>>));

  EXPECT_EQ(
      sizeof(MockTable) + sizeof(StatefulHash),
      sizeof(
          raw_hash_set<StringPolicy, StatefulHash,
                       std::equal_to<absl::string_view>, std::allocator<int>>));
}

TEST(Table, Empty) {
  IntTable t;
  EXPECT_EQ(0, t.size());
  EXPECT_TRUE(t.empty());
}

#ifdef __GNUC__
template <class T>
ABSL_ATTRIBUTE_ALWAYS_INLINE inline void DoNotOptimize(const T& v) {
  asm volatile("" : : "r,m"(v) : "memory");
}
#endif

TEST(Table, Prefetch) {
  IntTable t;
  t.emplace(1);
  // Works for both present and absent keys.
  t.prefetch(1);
  t.prefetch(2);

  // Do not run in debug mode, when prefetch is not implemented, or when
  // sanitizers are enabled.
#if defined(NDEBUG) && defined(__GNUC__) && !defined(ADDRESS_SANITIZER) && \
    !defined(MEMORY_SANITIZER) && !defined(THREAD_SANITIZER) &&            \
    !defined(UNDEFINED_BEHAVIOR_SANITIZER)
  const auto now = [] { return absl::base_internal::CycleClock::Now(); };

  static constexpr int size = 1000000;
  for (int i = 0; i < size; ++i) t.insert(i);

  int64_t no_prefetch = 0, prefetch = 0;
  for (int iter = 0; iter < 10; ++iter) {
    int64_t time = now();
    for (int i = 0; i < size; ++i) {
      DoNotOptimize(t.find(i));
    }
    no_prefetch += now() - time;

    time = now();
    for (int i = 0; i < size; ++i) {
      t.prefetch(i + 20);
      DoNotOptimize(t.find(i));
    }
    prefetch += now() - time;
  }

  // no_prefetch is at least 30% slower.
  EXPECT_GE(1.0 * no_prefetch / prefetch, 1.3);
#endif
}

TEST(Table, LookupEmpty) {
  IntTable t;
  auto it = t.find(0);
  EXPECT_TRUE(it == t.end());
}

TEST(Table, Insert1) {
  IntTable t;
  EXPECT_TRUE(t.find(0) == t.end());
  auto res = t.emplace(0);
  EXPECT_TRUE(res.second);
  EXPECT_THAT(*res.first, 0);
  EXPECT_EQ(1, t.size());
  EXPECT_THAT(*t.find(0), 0);
}

TEST(Table, Insert2) {
  IntTable t;
  EXPECT_TRUE(t.find(0) == t.end());
  auto res = t.emplace(0);
  EXPECT_TRUE(res.second);
  EXPECT_THAT(*res.first, 0);
  EXPECT_EQ(1, t.size());
  EXPECT_TRUE(t.find(1) == t.end());
  res = t.emplace(1);
  EXPECT_TRUE(res.second);
  EXPECT_THAT(*res.first, 1);
  EXPECT_EQ(2, t.size());
  EXPECT_THAT(*t.find(0), 0);
  EXPECT_THAT(*t.find(1), 1);
}

TEST(Table, InsertCollision) {
  BadTable t;
  EXPECT_TRUE(t.find(1) == t.end());
  auto res = t.emplace(1);
  EXPECT_TRUE(res.second);
  EXPECT_THAT(*res.first, 1);
  EXPECT_EQ(1, t.size());

  EXPECT_TRUE(t.find(2) == t.end());
  res = t.emplace(2);
  EXPECT_THAT(*res.first, 2);
  EXPECT_TRUE(res.second);
  EXPECT_EQ(2, t.size());

  EXPECT_THAT(*t.find(1), 1);
  EXPECT_THAT(*t.find(2), 2);
}

// Test that we do not add existent element in case we need to search through
// many groups with deleted elements
TEST(Table, InsertCollisionAndFindAfterDelete) {
  BadTable t;  // all elements go to the same group.
  // Have at least 2 groups with Group::kWidth collisions
  // plus some extra collisions in the last group.
  constexpr size_t kNumInserts = Group::kWidth * 2 + 5;
  for (size_t i = 0; i < kNumInserts; ++i) {
    auto res = t.emplace(i);
    EXPECT_TRUE(res.second);
    EXPECT_THAT(*res.first, i);
    EXPECT_EQ(i + 1, t.size());
  }

  // Remove elements one by one and check
  // that we still can find all other elements.
  for (size_t i = 0; i < kNumInserts; ++i) {
    EXPECT_EQ(1, t.erase(i)) << i;
    for (size_t j = i + 1; j < kNumInserts; ++j) {
      EXPECT_THAT(*t.find(j), j);
      auto res = t.emplace(j);
      EXPECT_FALSE(res.second) << i << " " << j;
      EXPECT_THAT(*res.first, j);
      EXPECT_EQ(kNumInserts - i - 1, t.size());
    }
  }
  EXPECT_TRUE(t.empty());
}

TEST(Table, LazyEmplace) {
  StringTable t;
  bool called = false;
  auto it = t.lazy_emplace("abc", [&](const StringTable::constructor& f) {
    called = true;
    f("abc", "ABC");
  });
  EXPECT_TRUE(called);
  EXPECT_THAT(*it, Pair("abc", "ABC"));
  called = false;
  it = t.lazy_emplace("abc", [&](const StringTable::constructor& f) {
    called = true;
    f("abc", "DEF");
  });
  EXPECT_FALSE(called);
  EXPECT_THAT(*it, Pair("abc", "ABC"));
}

TEST(Table, ContainsEmpty) {
  IntTable t;

  EXPECT_FALSE(t.contains(0));
}

TEST(Table, Contains1) {
  IntTable t;

  EXPECT_TRUE(t.insert(0).second);
  EXPECT_TRUE(t.contains(0));
  EXPECT_FALSE(t.contains(1));

  EXPECT_EQ(1, t.erase(0));
  EXPECT_FALSE(t.contains(0));
}

TEST(Table, Contains2) {
  IntTable t;

  EXPECT_TRUE(t.insert(0).second);
  EXPECT_TRUE(t.contains(0));
  EXPECT_FALSE(t.contains(1));

  t.clear();
  EXPECT_FALSE(t.contains(0));
}

int decompose_constructed;
struct DecomposeType {
  DecomposeType(int i) : i(i) {  // NOLINT
    ++decompose_constructed;
  }

  explicit DecomposeType(const char* d) : DecomposeType(*d) {}

  int i;
};

struct DecomposeHash {
  using is_transparent = void;
  size_t operator()(DecomposeType a) const { return a.i; }
  size_t operator()(int a) const { return a; }
  size_t operator()(const char* a) const { return *a; }
};

struct DecomposeEq {
  using is_transparent = void;
  bool operator()(DecomposeType a, DecomposeType b) const { return a.i == b.i; }
  bool operator()(DecomposeType a, int b) const { return a.i == b; }
  bool operator()(DecomposeType a, const char* b) const { return a.i == *b; }
};

struct DecomposePolicy {
  using slot_type = DecomposeType;
  using key_type = DecomposeType;
  using init_type = DecomposeType;

  template <typename T>
  static void construct(void*, DecomposeType* slot, T&& v) {
    *slot = DecomposeType(std::forward<T>(v));
  }
  static void destroy(void*, DecomposeType*) {}
  static DecomposeType& element(slot_type* slot) { return *slot; }

  template <class F, class T>
  static auto apply(F&& f, const T& x) -> decltype(std::forward<F>(f)(x, x)) {
    return std::forward<F>(f)(x, x);
  }
};

template <typename Hash, typename Eq>
void TestDecompose(bool construct_three) {
  DecomposeType elem{0};
  const int one = 1;
  const char* three_p = "3";
  const auto& three = three_p;

  raw_hash_set<DecomposePolicy, Hash, Eq, std::allocator<int>> set1;

  decompose_constructed = 0;
  int expected_constructed = 0;
  EXPECT_EQ(expected_constructed, decompose_constructed);
  set1.insert(elem);
  EXPECT_EQ(expected_constructed, decompose_constructed);
  set1.insert(1);
  EXPECT_EQ(++expected_constructed, decompose_constructed);
  set1.emplace("3");
  EXPECT_EQ(++expected_constructed, decompose_constructed);
  EXPECT_EQ(expected_constructed, decompose_constructed);

  {  // insert(T&&)
    set1.insert(1);
    EXPECT_EQ(expected_constructed, decompose_constructed);
  }

  {  // insert(const T&)
    set1.insert(one);
    EXPECT_EQ(expected_constructed, decompose_constructed);
  }

  {  // insert(hint, T&&)
    set1.insert(set1.begin(), 1);
    EXPECT_EQ(expected_constructed, decompose_constructed);
  }

  {  // insert(hint, const T&)
    set1.insert(set1.begin(), one);
    EXPECT_EQ(expected_constructed, decompose_constructed);
  }

  {  // emplace(...)
    set1.emplace(1);
    EXPECT_EQ(expected_constructed, decompose_constructed);
    set1.emplace("3");
    expected_constructed += construct_three;
    EXPECT_EQ(expected_constructed, decompose_constructed);
    set1.emplace(one);
    EXPECT_EQ(expected_constructed, decompose_constructed);
    set1.emplace(three);
    expected_constructed += construct_three;
    EXPECT_EQ(expected_constructed, decompose_constructed);
  }

  {  // emplace_hint(...)
    set1.emplace_hint(set1.begin(), 1);
    EXPECT_EQ(expected_constructed, decompose_constructed);
    set1.emplace_hint(set1.begin(), "3");
    expected_constructed += construct_three;
    EXPECT_EQ(expected_constructed, decompose_constructed);
    set1.emplace_hint(set1.begin(), one);
    EXPECT_EQ(expected_constructed, decompose_constructed);
    set1.emplace_hint(set1.begin(), three);
    expected_constructed += construct_three;
    EXPECT_EQ(expected_constructed, decompose_constructed);
  }
}

TEST(Table, Decompose) {
  TestDecompose<DecomposeHash, DecomposeEq>(false);

  struct TransparentHashIntOverload {
    size_t operator()(DecomposeType a) const { return a.i; }
    size_t operator()(int a) const { return a; }
  };
  struct TransparentEqIntOverload {
    bool operator()(DecomposeType a, DecomposeType b) const {
      return a.i == b.i;
    }
    bool operator()(DecomposeType a, int b) const { return a.i == b; }
  };
  TestDecompose<TransparentHashIntOverload, DecomposeEq>(true);
  TestDecompose<TransparentHashIntOverload, TransparentEqIntOverload>(true);
  TestDecompose<DecomposeHash, TransparentEqIntOverload>(true);
}

// Returns the largest m such that a table with m elements has the same number
// of buckets as a table with n elements.
size_t MaxDensitySize(size_t n) {
  IntTable t;
  t.reserve(n);
  for (size_t i = 0; i != n; ++i) t.emplace(i);
  const size_t c = t.bucket_count();
  while (c == t.bucket_count()) t.emplace(n++);
  return t.size() - 1;
}

struct Modulo1000Hash {
  size_t operator()(int x) const { return x % 1000; }
};

struct Modulo1000HashTable
    : public raw_hash_set<IntPolicy, Modulo1000Hash, std::equal_to<int>,
                          std::allocator<int>> {};

// Test that rehash with no resize happen in case of many deleted slots.
TEST(Table, RehashWithNoResize) {
  Modulo1000HashTable t;
  // Adding the same length (and the same hash) strings
  // to have at least kMinFullGroups groups
  // with Group::kWidth collisions. Then fill up to MaxDensitySize;
  const size_t kMinFullGroups = 7;
  std::vector<int> keys;
  for (size_t i = 0; i < MaxDensitySize(Group::kWidth * kMinFullGroups); ++i) {
    int k = i * 1000;
    t.emplace(k);
    keys.push_back(k);
  }
  const size_t capacity = t.capacity();

  // Remove elements from all groups except the first and the last one.
  // All elements removed from full groups will be marked as kDeleted.
  const size_t erase_begin = Group::kWidth / 2;
  const size_t erase_end = (t.size() / Group::kWidth - 1) * Group::kWidth;
  for (size_t i = erase_begin; i < erase_end; ++i) {
    EXPECT_EQ(1, t.erase(keys[i])) << i;
  }
  keys.erase(keys.begin() + erase_begin, keys.begin() + erase_end);

  auto last_key = keys.back();
  size_t last_key_num_probes = GetHashtableDebugNumProbes(t, last_key);

  // Make sure that we have to make a lot of probes for last key.
  ASSERT_GT(last_key_num_probes, kMinFullGroups);

  int x = 1;
  // Insert and erase one element, before inplace rehash happen.
  while (last_key_num_probes == GetHashtableDebugNumProbes(t, last_key)) {
    t.emplace(x);
    ASSERT_EQ(capacity, t.capacity());
    // All elements should be there.
    ASSERT_TRUE(t.find(x) != t.end()) << x;
    for (const auto& k : keys) {
      ASSERT_TRUE(t.find(k) != t.end()) << k;
    }
    t.erase(x);
    ++x;
  }
}

TEST(Table, InsertEraseStressTest) {
  IntTable t;
  const size_t kMinElementCount = 250;
  std::deque<int> keys;
  size_t i = 0;
  for (; i < MaxDensitySize(kMinElementCount); ++i) {
    t.emplace(i);
    keys.push_back(i);
  }
  const size_t kNumIterations = 1000000;
  for (; i < kNumIterations; ++i) {
    ASSERT_EQ(1, t.erase(keys.front()));
    keys.pop_front();
    t.emplace(i);
    keys.push_back(i);
  }
}

TEST(Table, InsertOverloads) {
  StringTable t;
  // These should all trigger the insert(init_type) overload.
  t.insert({{}, {}});
  t.insert({"ABC", {}});
  t.insert({"DEF", "!!!"});

  EXPECT_THAT(t, UnorderedElementsAre(Pair("", ""), Pair("ABC", ""),
                                      Pair("DEF", "!!!")));
}

TEST(Table, LargeTable) {
  IntTable t;
  for (int64_t i = 0; i != 100000; ++i) t.emplace(i << 40);
  for (int64_t i = 0; i != 100000; ++i) ASSERT_EQ(i << 40, *t.find(i << 40));
}

// Timeout if copy is quadratic as it was in Rust.
TEST(Table, EnsureNonQuadraticAsInRust) {
  static const size_t kLargeSize = 1 << 15;

  IntTable t;
  for (size_t i = 0; i != kLargeSize; ++i) {
    t.insert(i);
  }

  // If this is quadratic, the test will timeout.
  IntTable t2;
  for (const auto& entry : t) t2.insert(entry);
}

TEST(Table, ClearBug) {
  IntTable t;
  constexpr size_t capacity = container_internal::Group::kWidth - 1;
  constexpr size_t max_size = capacity / 2;
  for (size_t i = 0; i < max_size; ++i) {
    t.insert(i);
  }
  ASSERT_EQ(capacity, t.capacity());
  intptr_t original = reinterpret_cast<intptr_t>(&*t.find(2));
  t.clear();
  ASSERT_EQ(capacity, t.capacity());
  for (size_t i = 0; i < max_size; ++i) {
    t.insert(i);
  }
  ASSERT_EQ(capacity, t.capacity());
  intptr_t second = reinterpret_cast<intptr_t>(&*t.find(2));
  // We are checking that original and second are close enough to each other
  // that they are probably still in the same group.  This is not strictly
  // guaranteed.
  EXPECT_LT(std::abs(original - second),
            capacity * sizeof(IntTable::value_type));
}

TEST(Table, Erase) {
  IntTable t;
  EXPECT_TRUE(t.find(0) == t.end());
  auto res = t.emplace(0);
  EXPECT_TRUE(res.second);
  EXPECT_EQ(1, t.size());
  t.erase(res.first);
  EXPECT_EQ(0, t.size());
  EXPECT_TRUE(t.find(0) == t.end());
}

// Collect N bad keys by following algorithm:
// 1. Create an empty table and reserve it to 2 * N.
// 2. Insert N random elements.
// 3. Take first Group::kWidth - 1 to bad_keys array.
// 4. Clear the table without resize.
// 5. Go to point 2 while N keys not collected
std::vector<int64_t> CollectBadMergeKeys(size_t N) {
  static constexpr int kGroupSize = Group::kWidth - 1;

  auto topk_range = [](size_t b, size_t e, IntTable* t) -> std::vector<int64_t> {
    for (size_t i = b; i != e; ++i) {
      t->emplace(i);
    }
    std::vector<int64_t> res;
    res.reserve(kGroupSize);
    auto it = t->begin();
    for (size_t i = b; i != e && i != b + kGroupSize; ++i, ++it) {
      res.push_back(*it);
    }
    return res;
  };

  std::vector<int64_t> bad_keys;
  bad_keys.reserve(N);
  IntTable t;
  t.reserve(N * 2);

  for (size_t b = 0; bad_keys.size() < N; b += N) {
    auto keys = topk_range(b, b + N, &t);
    bad_keys.insert(bad_keys.end(), keys.begin(), keys.end());
    t.erase(t.begin(), t.end());
    EXPECT_TRUE(t.empty());
  }
  return bad_keys;
}

struct ProbeStats {
  // Number of elements with specific probe length over all tested tables.
  std::vector<size_t> all_probes_histogram;
  // Ratios total_probe_length/size for every tested table.
  std::vector<double> single_table_ratios;

  friend ProbeStats operator+(const ProbeStats& a, const ProbeStats& b) {
    ProbeStats res = a;
    res.all_probes_histogram.resize(std::max(res.all_probes_histogram.size(),
                                             b.all_probes_histogram.size()));
    std::transform(b.all_probes_histogram.begin(), b.all_probes_histogram.end(),
                   res.all_probes_histogram.begin(),
                   res.all_probes_histogram.begin(), std::plus<size_t>());
    res.single_table_ratios.insert(res.single_table_ratios.end(),
                                   b.single_table_ratios.begin(),
                                   b.single_table_ratios.end());
    return res;
  }

  // Average ratio total_probe_length/size over tables.
  double AvgRatio() const {
    return std::accumulate(single_table_ratios.begin(),
                           single_table_ratios.end(), 0.0) /
           single_table_ratios.size();
  }

  // Maximum ratio total_probe_length/size over tables.
  double MaxRatio() const {
    return *std::max_element(single_table_ratios.begin(),
                             single_table_ratios.end());
  }

  // Percentile ratio total_probe_length/size over tables.
  double PercentileRatio(double Percentile = 0.95) const {
    auto r = single_table_ratios;
    auto mid = r.begin() + static_cast<size_t>(r.size() * Percentile);
    if (mid != r.end()) {
      std::nth_element(r.begin(), mid, r.end());
      return *mid;
    } else {
      return MaxRatio();
    }
  }

  // Maximum probe length over all elements and all tables.
  size_t MaxProbe() const { return all_probes_histogram.size(); }

  // Fraction of elements with specified probe length.
  std::vector<double> ProbeNormalizedHistogram() const {
    double total_elements = std::accumulate(all_probes_histogram.begin(),
                                            all_probes_histogram.end(), 0ull);
    std::vector<double> res;
    for (size_t p : all_probes_histogram) {
      res.push_back(p / total_elements);
    }
    return res;
  }

  size_t PercentileProbe(double Percentile = 0.99) const {
    size_t idx = 0;
    for (double p : ProbeNormalizedHistogram()) {
      if (Percentile > p) {
        Percentile -= p;
        ++idx;
      } else {
        return idx;
      }
    }
    return idx;
  }

  friend std::ostream& operator<<(std::ostream& out, const ProbeStats& s) {
    out << "{AvgRatio:" << s.AvgRatio() << ", MaxRatio:" << s.MaxRatio()
        << ", PercentileRatio:" << s.PercentileRatio()
        << ", MaxProbe:" << s.MaxProbe() << ", Probes=[";
    for (double p : s.ProbeNormalizedHistogram()) {
      out << p << ",";
    }
    out << "]}";

    return out;
  }
};

struct ExpectedStats {
  double avg_ratio;
  double max_ratio;
  std::vector<std::pair<double, double>> pecentile_ratios;
  std::vector<std::pair<double, double>> pecentile_probes;

  friend std::ostream& operator<<(std::ostream& out, const ExpectedStats& s) {
    out << "{AvgRatio:" << s.avg_ratio << ", MaxRatio:" << s.max_ratio
        << ", PercentileRatios: [";
    for (auto el : s.pecentile_ratios) {
      out << el.first << ":" << el.second << ", ";
    }
    out << "], PercentileProbes: [";
    for (auto el : s.pecentile_probes) {
      out << el.first << ":" << el.second << ", ";
    }
    out << "]}";

    return out;
  }
};

void VerifyStats(size_t size, const ExpectedStats& exp,
                 const ProbeStats& stats) {
  EXPECT_LT(stats.AvgRatio(), exp.avg_ratio) << size << " " << stats;
  EXPECT_LT(stats.MaxRatio(), exp.max_ratio) << size << " " << stats;
  for (auto pr : exp.pecentile_ratios) {
    EXPECT_LE(stats.PercentileRatio(pr.first), pr.second)
        << size << " " << pr.first << " " << stats;
  }

  for (auto pr : exp.pecentile_probes) {
    EXPECT_LE(stats.PercentileProbe(pr.first), pr.second)
        << size << " " << pr.first << " " << stats;
  }
}

using ProbeStatsPerSize = std::map<size_t, ProbeStats>;

// Collect total ProbeStats on num_iters iterations of the following algorithm:
// 1. Create new table and reserve it to keys.size() * 2
// 2. Insert all keys xored with seed
// 3. Collect ProbeStats from final table.
ProbeStats CollectProbeStatsOnKeysXoredWithSeed(const std::vector<int64_t>& keys,
                                                size_t num_iters) {
  const size_t reserve_size = keys.size() * 2;

  ProbeStats stats;

  int64_t seed = 0x71b1a19b907d6e33;
  while (num_iters--) {
    seed = static_cast<int64_t>(static_cast<uint64_t>(seed) * 17 + 13);
    IntTable t1;
    t1.reserve(reserve_size);
    for (const auto& key : keys) {
      t1.emplace(key ^ seed);
    }

    auto probe_histogram = GetHashtableDebugNumProbesHistogram(t1);
    stats.all_probes_histogram.resize(
        std::max(stats.all_probes_histogram.size(), probe_histogram.size()));
    std::transform(probe_histogram.begin(), probe_histogram.end(),
                   stats.all_probes_histogram.begin(),
                   stats.all_probes_histogram.begin(), std::plus<size_t>());

    size_t total_probe_seq_length = 0;
    for (size_t i = 0; i < probe_histogram.size(); ++i) {
      total_probe_seq_length += i * probe_histogram[i];
    }
    stats.single_table_ratios.push_back(total_probe_seq_length * 1.0 /
                                        keys.size());
    t1.erase(t1.begin(), t1.end());
  }
  return stats;
}

ExpectedStats XorSeedExpectedStats() {
  constexpr bool kRandomizesInserts =
#if NDEBUG
      false;
#else   // NDEBUG
      true;
#endif  // NDEBUG

  // The effective load factor is larger in non-opt mode because we insert
  // elements out of order.
  switch (container_internal::Group::kWidth) {
    case 8:
      if (kRandomizesInserts) {
  return {0.05,
          1.0,
          {{0.95, 0.5}},
          {{0.95, 0}, {0.99, 2}, {0.999, 4}, {0.9999, 10}}};
      } else {
  return {0.05,
          2.0,
          {{0.95, 0.1}},
          {{0.95, 0}, {0.99, 2}, {0.999, 4}, {0.9999, 10}}};
      }
    case 16:
      if (kRandomizesInserts) {
        return {0.1,
                1.0,
                {{0.95, 0.1}},
                {{0.95, 0}, {0.99, 1}, {0.999, 8}, {0.9999, 15}}};
      } else {
        return {0.05,
                1.0,
                {{0.95, 0.05}},
                {{0.95, 0}, {0.99, 1}, {0.999, 4}, {0.9999, 10}}};
      }
  }
  ABSL_RAW_LOG(FATAL, "%s", "Unknown Group width");
  return {};
}
TEST(Table, DISABLED_EnsureNonQuadraticTopNXorSeedByProbeSeqLength) {
  ProbeStatsPerSize stats;
  std::vector<size_t> sizes = {Group::kWidth << 5, Group::kWidth << 10};
  for (size_t size : sizes) {
    stats[size] =
        CollectProbeStatsOnKeysXoredWithSeed(CollectBadMergeKeys(size), 200);
  }
  auto expected = XorSeedExpectedStats();
  for (size_t size : sizes) {
    auto& stat = stats[size];
    VerifyStats(size, expected, stat);
  }
}

// Collect total ProbeStats on num_iters iterations of the following algorithm:
// 1. Create new table
// 2. Select 10% of keys and insert 10 elements key * 17 + j * 13
// 3. Collect ProbeStats from final table
ProbeStats CollectProbeStatsOnLinearlyTransformedKeys(
    const std::vector<int64_t>& keys, size_t num_iters) {
  ProbeStats stats;

  std::random_device rd;
  std::mt19937 rng(rd());
  auto linear_transform = [](size_t x, size_t y) { return x * 17 + y * 13; };
  std::uniform_int_distribution<size_t> dist(0, keys.size()-1);
  while (num_iters--) {
    IntTable t1;
    size_t num_keys = keys.size() / 10;
    size_t start = dist(rng);
    for (size_t i = 0; i != num_keys; ++i) {
      for (size_t j = 0; j != 10; ++j) {
        t1.emplace(linear_transform(keys[(i + start) % keys.size()], j));
      }
    }

    auto probe_histogram = GetHashtableDebugNumProbesHistogram(t1);
    stats.all_probes_histogram.resize(
        std::max(stats.all_probes_histogram.size(), probe_histogram.size()));
    std::transform(probe_histogram.begin(), probe_histogram.end(),
                   stats.all_probes_histogram.begin(),
                   stats.all_probes_histogram.begin(), std::plus<size_t>());

    size_t total_probe_seq_length = 0;
    for (size_t i = 0; i < probe_histogram.size(); ++i) {
      total_probe_seq_length += i * probe_histogram[i];
    }
    stats.single_table_ratios.push_back(total_probe_seq_length * 1.0 /
                                        t1.size());
    t1.erase(t1.begin(), t1.end());
  }
  return stats;
}

ExpectedStats LinearTransformExpectedStats() {
  constexpr bool kRandomizesInserts =
#if NDEBUG
      false;
#else   // NDEBUG
      true;
#endif  // NDEBUG

  // The effective load factor is larger in non-opt mode because we insert
  // elements out of order.
  switch (container_internal::Group::kWidth) {
    case 8:
      if (kRandomizesInserts) {
        return {0.1,
                0.5,
                {{0.95, 0.3}},
                {{0.95, 0}, {0.99, 1}, {0.999, 8}, {0.9999, 15}}};
      } else {
        return {0.15,
                0.5,
                {{0.95, 0.3}},
                {{0.95, 0}, {0.99, 3}, {0.999, 15}, {0.9999, 25}}};
      }
    case 16:
      if (kRandomizesInserts) {
        return {0.1,
                0.4,
                {{0.95, 0.3}},
                {{0.95, 0}, {0.99, 1}, {0.999, 8}, {0.9999, 15}}};
      } else {
        return {0.05,
                0.2,
                {{0.95, 0.1}},
                {{0.95, 0}, {0.99, 1}, {0.999, 6}, {0.9999, 10}}};
      }
  }
  ABSL_RAW_LOG(FATAL, "%s", "Unknown Group width");
  return {};
}
TEST(Table, DISABLED_EnsureNonQuadraticTopNLinearTransformByProbeSeqLength) {
  ProbeStatsPerSize stats;
  std::vector<size_t> sizes = {Group::kWidth << 5, Group::kWidth << 10};
  for (size_t size : sizes) {
    stats[size] = CollectProbeStatsOnLinearlyTransformedKeys(
        CollectBadMergeKeys(size), 300);
  }
  auto expected = LinearTransformExpectedStats();
  for (size_t size : sizes) {
    auto& stat = stats[size];
    VerifyStats(size, expected, stat);
  }
}

TEST(Table, EraseCollision) {
  BadTable t;

  // 1 2 3
  t.emplace(1);
  t.emplace(2);
  t.emplace(3);
  EXPECT_THAT(*t.find(1), 1);
  EXPECT_THAT(*t.find(2), 2);
  EXPECT_THAT(*t.find(3), 3);
  EXPECT_EQ(3, t.size());

  // 1 DELETED 3
  t.erase(t.find(2));
  EXPECT_THAT(*t.find(1), 1);
  EXPECT_TRUE(t.find(2) == t.end());
  EXPECT_THAT(*t.find(3), 3);
  EXPECT_EQ(2, t.size());

  // DELETED DELETED 3
  t.erase(t.find(1));
  EXPECT_TRUE(t.find(1) == t.end());
  EXPECT_TRUE(t.find(2) == t.end());
  EXPECT_THAT(*t.find(3), 3);
  EXPECT_EQ(1, t.size());

  // DELETED DELETED DELETED
  t.erase(t.find(3));
  EXPECT_TRUE(t.find(1) == t.end());
  EXPECT_TRUE(t.find(2) == t.end());
  EXPECT_TRUE(t.find(3) == t.end());
  EXPECT_EQ(0, t.size());
}

TEST(Table, EraseInsertProbing) {
  BadTable t(100);

  // 1 2 3 4
  t.emplace(1);
  t.emplace(2);
  t.emplace(3);
  t.emplace(4);

  // 1 DELETED 3 DELETED
  t.erase(t.find(2));
  t.erase(t.find(4));

  // 1 10 3 11 12
  t.emplace(10);
  t.emplace(11);
  t.emplace(12);

  EXPECT_EQ(5, t.size());
  EXPECT_THAT(t, UnorderedElementsAre(1, 10, 3, 11, 12));
}

TEST(Table, Clear) {
  IntTable t;
  EXPECT_TRUE(t.find(0) == t.end());
  t.clear();
  EXPECT_TRUE(t.find(0) == t.end());
  auto res = t.emplace(0);
  EXPECT_TRUE(res.second);
  EXPECT_EQ(1, t.size());
  t.clear();
  EXPECT_EQ(0, t.size());
  EXPECT_TRUE(t.find(0) == t.end());
}

TEST(Table, Swap) {
  IntTable t;
  EXPECT_TRUE(t.find(0) == t.end());
  auto res = t.emplace(0);
  EXPECT_TRUE(res.second);
  EXPECT_EQ(1, t.size());
  IntTable u;
  t.swap(u);
  EXPECT_EQ(0, t.size());
  EXPECT_EQ(1, u.size());
  EXPECT_TRUE(t.find(0) == t.end());
  EXPECT_THAT(*u.find(0), 0);
}

TEST(Table, Rehash) {
  IntTable t;
  EXPECT_TRUE(t.find(0) == t.end());
  t.emplace(0);
  t.emplace(1);
  EXPECT_EQ(2, t.size());
  t.rehash(128);
  EXPECT_EQ(2, t.size());
  EXPECT_THAT(*t.find(0), 0);
  EXPECT_THAT(*t.find(1), 1);
}

TEST(Table, RehashDoesNotRehashWhenNotNecessary) {
  IntTable t;
  t.emplace(0);
  t.emplace(1);
  auto* p = &*t.find(0);
  t.rehash(1);
  EXPECT_EQ(p, &*t.find(0));
}

TEST(Table, RehashZeroDoesNotAllocateOnEmptyTable) {
  IntTable t;
  t.rehash(0);
  EXPECT_EQ(0, t.bucket_count());
}

TEST(Table, RehashZeroDeallocatesEmptyTable) {
  IntTable t;
  t.emplace(0);
  t.clear();
  EXPECT_NE(0, t.bucket_count());
  t.rehash(0);
  EXPECT_EQ(0, t.bucket_count());
}

TEST(Table, RehashZeroForcesRehash) {
  IntTable t;
  t.emplace(0);
  t.emplace(1);
  auto* p = &*t.find(0);
  t.rehash(0);
  EXPECT_NE(p, &*t.find(0));
}

TEST(Table, ConstructFromInitList) {
  using P = std::pair<std::string, std::string>;
  struct Q {
    operator P() const { return {}; }
  };
  StringTable t = {P(), Q(), {}, {{}, {}}};
}

TEST(Table, CopyConstruct) {
  IntTable t;
  t.max_load_factor(.321f);
  t.emplace(0);
  EXPECT_EQ(1, t.size());
  {
    IntTable u(t);
    EXPECT_EQ(1, u.size());
    EXPECT_EQ(t.max_load_factor(), u.max_load_factor());
    EXPECT_THAT(*u.find(0), 0);
  }
  {
    IntTable u{t};
    EXPECT_EQ(1, u.size());
    EXPECT_EQ(t.max_load_factor(), u.max_load_factor());
    EXPECT_THAT(*u.find(0), 0);
  }
  {
    IntTable u = t;
    EXPECT_EQ(1, u.size());
    EXPECT_EQ(t.max_load_factor(), u.max_load_factor());
    EXPECT_THAT(*u.find(0), 0);
  }
}

TEST(Table, CopyConstructWithAlloc) {
  StringTable t;
  t.max_load_factor(.321f);
  t.emplace("a", "b");
  EXPECT_EQ(1, t.size());
  StringTable u(t, Alloc<std::pair<std::string, std::string>>());
  EXPECT_EQ(1, u.size());
  EXPECT_EQ(t.max_load_factor(), u.max_load_factor());
  EXPECT_THAT(*u.find("a"), Pair("a", "b"));
}

struct ExplicitAllocIntTable
    : raw_hash_set<IntPolicy, container_internal::hash_default_hash<int64_t>,
                   std::equal_to<int64_t>, Alloc<int64_t>> {
  ExplicitAllocIntTable() {}
};

TEST(Table, AllocWithExplicitCtor) {
  ExplicitAllocIntTable t;
  EXPECT_EQ(0, t.size());
}

TEST(Table, MoveConstruct) {
  {
    StringTable t;
    t.max_load_factor(.321f);
    const float lf = t.max_load_factor();
    t.emplace("a", "b");
    EXPECT_EQ(1, t.size());

    StringTable u(std::move(t));
    EXPECT_EQ(1, u.size());
    EXPECT_EQ(lf, u.max_load_factor());
    EXPECT_THAT(*u.find("a"), Pair("a", "b"));
  }
  {
    StringTable t;
    t.max_load_factor(.321f);
    const float lf = t.max_load_factor();
    t.emplace("a", "b");
    EXPECT_EQ(1, t.size());

    StringTable u{std::move(t)};
    EXPECT_EQ(1, u.size());
    EXPECT_EQ(lf, u.max_load_factor());
    EXPECT_THAT(*u.find("a"), Pair("a", "b"));
  }
  {
    StringTable t;
    t.max_load_factor(.321f);
    const float lf = t.max_load_factor();
    t.emplace("a", "b");
    EXPECT_EQ(1, t.size());

    StringTable u = std::move(t);
    EXPECT_EQ(1, u.size());
    EXPECT_EQ(lf, u.max_load_factor());
    EXPECT_THAT(*u.find("a"), Pair("a", "b"));
  }
}

TEST(Table, MoveConstructWithAlloc) {
  StringTable t;
  t.max_load_factor(.321f);
  const float lf = t.max_load_factor();
  t.emplace("a", "b");
  EXPECT_EQ(1, t.size());
  StringTable u(std::move(t), Alloc<std::pair<std::string, std::string>>());
  EXPECT_EQ(1, u.size());
  EXPECT_EQ(lf, u.max_load_factor());
  EXPECT_THAT(*u.find("a"), Pair("a", "b"));
}

TEST(Table, CopyAssign) {
  StringTable t;
  t.max_load_factor(.321f);
  t.emplace("a", "b");
  EXPECT_EQ(1, t.size());
  StringTable u;
  u = t;
  EXPECT_EQ(1, u.size());
  EXPECT_EQ(t.max_load_factor(), u.max_load_factor());
  EXPECT_THAT(*u.find("a"), Pair("a", "b"));
}

TEST(Table, CopySelfAssign) {
  StringTable t;
  t.max_load_factor(.321f);
  const float lf = t.max_load_factor();
  t.emplace("a", "b");
  EXPECT_EQ(1, t.size());
  t = *&t;
  EXPECT_EQ(1, t.size());
  EXPECT_EQ(lf, t.max_load_factor());
  EXPECT_THAT(*t.find("a"), Pair("a", "b"));
}

TEST(Table, MoveAssign) {
  StringTable t;
  t.max_load_factor(.321f);
  const float lf = t.max_load_factor();
  t.emplace("a", "b");
  EXPECT_EQ(1, t.size());
  StringTable u;
  u = std::move(t);
  EXPECT_EQ(1, u.size());
  EXPECT_EQ(lf, u.max_load_factor());
  EXPECT_THAT(*u.find("a"), Pair("a", "b"));
}

TEST(Table, Equality) {
  StringTable t;
  std::vector<std::pair<std::string, std::string>> v = {{"a", "b"}, {"aa", "bb"}};
  t.insert(std::begin(v), std::end(v));
  StringTable u = t;
  EXPECT_EQ(u, t);
}

TEST(Table, Equality2) {
  StringTable t;
  std::vector<std::pair<std::string, std::string>> v1 = {{"a", "b"}, {"aa", "bb"}};
  t.insert(std::begin(v1), std::end(v1));
  StringTable u;
  std::vector<std::pair<std::string, std::string>> v2 = {{"a", "a"}, {"aa", "aa"}};
  u.insert(std::begin(v2), std::end(v2));
  EXPECT_NE(u, t);
}

TEST(Table, Equality3) {
  StringTable t;
  std::vector<std::pair<std::string, std::string>> v1 = {{"b", "b"}, {"bb", "bb"}};
  t.insert(std::begin(v1), std::end(v1));
  StringTable u;
  std::vector<std::pair<std::string, std::string>> v2 = {{"a", "a"}, {"aa", "aa"}};
  u.insert(std::begin(v2), std::end(v2));
  EXPECT_NE(u, t);
}

TEST(Table, NumDeletedRegression) {
  IntTable t;
  t.emplace(0);
  t.erase(t.find(0));
  // construct over a deleted slot.
  t.emplace(0);
  t.clear();
}

TEST(Table, FindFullDeletedRegression) {
  IntTable t;
  for (int i = 0; i < 1000; ++i) {
    t.emplace(i);
    t.erase(t.find(i));
  }
  EXPECT_EQ(0, t.size());
}

TEST(Table, ReplacingDeletedSlotDoesNotRehash) {
  size_t n;
  {
    // Compute n such that n is the maximum number of elements before rehash.
    IntTable t;
    t.emplace(0);
    size_t c = t.bucket_count();
    for (n = 1; c == t.bucket_count(); ++n) t.emplace(n);
    --n;
  }
  IntTable t;
  t.rehash(n);
  const size_t c = t.bucket_count();
  for (size_t i = 0; i != n; ++i) t.emplace(i);
  EXPECT_EQ(c, t.bucket_count()) << "rehashing threshold = " << n;
  t.erase(0);
  t.emplace(0);
  EXPECT_EQ(c, t.bucket_count()) << "rehashing threshold = " << n;
}

TEST(Table, NoThrowMoveConstruct) {
  ASSERT_TRUE(
      std::is_nothrow_copy_constructible<absl::Hash<absl::string_view>>::value);
  ASSERT_TRUE(std::is_nothrow_copy_constructible<
              std::equal_to<absl::string_view>>::value);
  ASSERT_TRUE(std::is_nothrow_copy_constructible<std::allocator<int>>::value);
  EXPECT_TRUE(std::is_nothrow_move_constructible<StringTable>::value);
}

TEST(Table, NoThrowMoveAssign) {
  ASSERT_TRUE(
      std::is_nothrow_move_assignable<absl::Hash<absl::string_view>>::value);
  ASSERT_TRUE(
      std::is_nothrow_move_assignable<std::equal_to<absl::string_view>>::value);
  ASSERT_TRUE(std::is_nothrow_move_assignable<std::allocator<int>>::value);
  ASSERT_TRUE(
      absl::allocator_traits<std::allocator<int>>::is_always_equal::value);
  EXPECT_TRUE(std::is_nothrow_move_assignable<StringTable>::value);
}

TEST(Table, NoThrowSwappable) {
  ASSERT_TRUE(
      container_internal::IsNoThrowSwappable<absl::Hash<absl::string_view>>());
  ASSERT_TRUE(container_internal::IsNoThrowSwappable<
              std::equal_to<absl::string_view>>());
  ASSERT_TRUE(container_internal::IsNoThrowSwappable<std::allocator<int>>());
  EXPECT_TRUE(container_internal::IsNoThrowSwappable<StringTable>());
}

TEST(Table, HeterogeneousLookup) {
  struct Hash {
    size_t operator()(int64_t i) const { return i; }
    size_t operator()(double i) const {
      ADD_FAILURE();
      return i;
    }
  };
  struct Eq {
    bool operator()(int64_t a, int64_t b) const { return a == b; }
    bool operator()(double a, int64_t b) const {
      ADD_FAILURE();
      return a == b;
    }
    bool operator()(int64_t a, double b) const {
      ADD_FAILURE();
      return a == b;
    }
    bool operator()(double a, double b) const {
      ADD_FAILURE();
      return a == b;
    }
  };

  struct THash {
    using is_transparent = void;
    size_t operator()(int64_t i) const { return i; }
    size_t operator()(double i) const { return i; }
  };
  struct TEq {
    using is_transparent = void;
    bool operator()(int64_t a, int64_t b) const { return a == b; }
    bool operator()(double a, int64_t b) const { return a == b; }
    bool operator()(int64_t a, double b) const { return a == b; }
    bool operator()(double a, double b) const { return a == b; }
  };

  raw_hash_set<IntPolicy, Hash, Eq, Alloc<int64_t>> s{0, 1, 2};
  // It will convert to int64_t before the query.
  EXPECT_EQ(1, *s.find(double{1.1}));

  raw_hash_set<IntPolicy, THash, TEq, Alloc<int64_t>> ts{0, 1, 2};
  // It will try to use the double, and fail to find the object.
  EXPECT_TRUE(ts.find(1.1) == ts.end());
}

template <class Table>
using CallFind = decltype(std::declval<Table&>().find(17));

template <class Table>
using CallErase = decltype(std::declval<Table&>().erase(17));

template <class Table>
using CallExtract = decltype(std::declval<Table&>().extract(17));

template <class Table>
using CallPrefetch = decltype(std::declval<Table&>().prefetch(17));

template <class Table>
using CallCount = decltype(std::declval<Table&>().count(17));

template <template <typename> class C, class Table, class = void>
struct VerifyResultOf : std::false_type {};

template <template <typename> class C, class Table>
struct VerifyResultOf<C, Table, absl::void_t<C<Table>>> : std::true_type {};

TEST(Table, HeterogeneousLookupOverloads) {
  using NonTransparentTable =
      raw_hash_set<StringPolicy, absl::Hash<absl::string_view>,
                   std::equal_to<absl::string_view>, std::allocator<int>>;

  EXPECT_FALSE((VerifyResultOf<CallFind, NonTransparentTable>()));
  EXPECT_FALSE((VerifyResultOf<CallErase, NonTransparentTable>()));
  EXPECT_FALSE((VerifyResultOf<CallExtract, NonTransparentTable>()));
  EXPECT_FALSE((VerifyResultOf<CallPrefetch, NonTransparentTable>()));
  EXPECT_FALSE((VerifyResultOf<CallCount, NonTransparentTable>()));

  using TransparentTable = raw_hash_set<
      StringPolicy,
      absl::container_internal::hash_default_hash<absl::string_view>,
      absl::container_internal::hash_default_eq<absl::string_view>,
      std::allocator<int>>;

  EXPECT_TRUE((VerifyResultOf<CallFind, TransparentTable>()));
  EXPECT_TRUE((VerifyResultOf<CallErase, TransparentTable>()));
  EXPECT_TRUE((VerifyResultOf<CallExtract, TransparentTable>()));
  EXPECT_TRUE((VerifyResultOf<CallPrefetch, TransparentTable>()));
  EXPECT_TRUE((VerifyResultOf<CallCount, TransparentTable>()));
}

// TODO(alkis): Expand iterator tests.
TEST(Iterator, IsDefaultConstructible) {
  StringTable::iterator i;
  EXPECT_TRUE(i == StringTable::iterator());
}

TEST(ConstIterator, IsDefaultConstructible) {
  StringTable::const_iterator i;
  EXPECT_TRUE(i == StringTable::const_iterator());
}

TEST(Iterator, ConvertsToConstIterator) {
  StringTable::iterator i;
  EXPECT_TRUE(i == StringTable::const_iterator());
}

TEST(Iterator, Iterates) {
  IntTable t;
  for (size_t i = 3; i != 6; ++i) EXPECT_TRUE(t.emplace(i).second);
  EXPECT_THAT(t, UnorderedElementsAre(3, 4, 5));
}

TEST(Table, Merge) {
  StringTable t1, t2;
  t1.emplace("0", "-0");
  t1.emplace("1", "-1");
  t2.emplace("0", "~0");
  t2.emplace("2", "~2");

  EXPECT_THAT(t1, UnorderedElementsAre(Pair("0", "-0"), Pair("1", "-1")));
  EXPECT_THAT(t2, UnorderedElementsAre(Pair("0", "~0"), Pair("2", "~2")));

  t1.merge(t2);
  EXPECT_THAT(t1, UnorderedElementsAre(Pair("0", "-0"), Pair("1", "-1"),
                                       Pair("2", "~2")));
  EXPECT_THAT(t2, UnorderedElementsAre(Pair("0", "~0")));
}

TEST(Nodes, EmptyNodeType) {
  using node_type = StringTable::node_type;
  node_type n;
  EXPECT_FALSE(n);
  EXPECT_TRUE(n.empty());

  EXPECT_TRUE((std::is_same<node_type::allocator_type,
                            StringTable::allocator_type>::value));
}

TEST(Nodes, ExtractInsert) {
  constexpr char k0[] = "Very long std::string zero.";
  constexpr char k1[] = "Very long std::string one.";
  constexpr char k2[] = "Very long std::string two.";
  StringTable t = {{k0, ""}, {k1, ""}, {k2, ""}};
  EXPECT_THAT(t,
              UnorderedElementsAre(Pair(k0, ""), Pair(k1, ""), Pair(k2, "")));

  auto node = t.extract(k0);
  EXPECT_THAT(t, UnorderedElementsAre(Pair(k1, ""), Pair(k2, "")));
  EXPECT_TRUE(node);
  EXPECT_FALSE(node.empty());

  StringTable t2;
  auto res = t2.insert(std::move(node));
  EXPECT_TRUE(res.inserted);
  EXPECT_THAT(*res.position, Pair(k0, ""));
  EXPECT_FALSE(res.node);
  EXPECT_THAT(t2, UnorderedElementsAre(Pair(k0, "")));

  // Not there.
  EXPECT_THAT(t, UnorderedElementsAre(Pair(k1, ""), Pair(k2, "")));
  node = t.extract("Not there!");
  EXPECT_THAT(t, UnorderedElementsAre(Pair(k1, ""), Pair(k2, "")));
  EXPECT_FALSE(node);

  // Inserting nothing.
  res = t2.insert(std::move(node));
  EXPECT_FALSE(res.inserted);
  EXPECT_EQ(res.position, t2.end());
  EXPECT_FALSE(res.node);
  EXPECT_THAT(t2, UnorderedElementsAre(Pair(k0, "")));

  t.emplace(k0, "1");
  node = t.extract(k0);

  // Insert duplicate.
  res = t2.insert(std::move(node));
  EXPECT_FALSE(res.inserted);
  EXPECT_THAT(*res.position, Pair(k0, ""));
  EXPECT_TRUE(res.node);
  EXPECT_FALSE(node);
}

StringTable MakeSimpleTable(size_t size) {
  StringTable t;
  for (size_t i = 0; i < size; ++i) t.emplace(std::string(1, 'A' + i), "");
  return t;
}

std::string OrderOfIteration(const StringTable& t) {
  std::string order;
  for (auto& p : t) order += p.first;
  return order;
}

TEST(Table, IterationOrderChangesByInstance) {
  // Needs to be more than kWidth elements to be able to affect order.
  const StringTable reference = MakeSimpleTable(20);

  // Since order is non-deterministic we can't just try once and verify.
  // We'll try until we find that order changed. It should not take many tries
  // for that.
  // Important: we have to keep the old tables around. Otherwise tcmalloc will
  // just give us the same blocks and we would be doing the same order again.
  std::vector<StringTable> garbage;
  for (int i = 0; i < 10; ++i) {
    auto trial = MakeSimpleTable(20);
    if (OrderOfIteration(trial) != OrderOfIteration(reference)) {
      // We are done.
      return;
    }
    garbage.push_back(std::move(trial));
  }
  FAIL();
}

TEST(Table, IterationOrderChangesOnRehash) {
  // Since order is non-deterministic we can't just try once and verify.
  // We'll try until we find that order changed. It should not take many tries
  // for that.
  // Important: we have to keep the old tables around. Otherwise tcmalloc will
  // just give us the same blocks and we would be doing the same order again.
  std::vector<StringTable> garbage;
  for (int i = 0; i < 10; ++i) {
    // Needs to be more than kWidth elements to be able to affect order.
    StringTable t = MakeSimpleTable(20);
    const std::string reference = OrderOfIteration(t);
    // Force rehash to the same size.
    t.rehash(0);
    std::string trial = OrderOfIteration(t);
    if (trial != reference) {
      // We are done.
      return;
    }
    garbage.push_back(std::move(t));
  }
  FAIL();
}

TEST(Table, IterationOrderChangesForSmallTables) {
  // Since order is non-deterministic we can't just try once and verify.
  // We'll try until we find that order changed.
  // Important: we have to keep the old tables around. Otherwise tcmalloc will
  // just give us the same blocks and we would be doing the same order again.
  StringTable reference_table = MakeSimpleTable(5);
  const std::string reference = OrderOfIteration(reference_table);
  std::vector<StringTable> garbage;
  for (int i = 0; i < 50; ++i) {
    StringTable t = MakeSimpleTable(5);
    std::string trial = OrderOfIteration(t);
    if (trial != reference) {
      // We are done.
      return;
    }
    garbage.push_back(std::move(t));
  }
  FAIL() << "Iteration order remained the same across many attempts.";
}

// Fill the table to 3 different load factors (min, median, max) and evaluate
// the percentage of perfect hits using the debug API.
template <class Table, class AddFn>
std::vector<double> CollectPerfectRatios(Table, AddFn add) {
  std::vector<double> results(3);

  constexpr size_t kNumTrials = 10;
  std::vector<Table> tables(kNumTrials);

  for (Table& t : tables) {
    using Key = typename Table::key_type;

    // First, fill enough to have a good distribution.
    constexpr size_t kMinSize = 10000;
    std::vector<Key> keys;
    while (t.size() < kMinSize) keys.push_back(add(t));
    // Then, insert until we reach min load factor.
    double lf = t.load_factor();
    while (lf <= t.load_factor()) keys.push_back(add(t));

    // We are now at min load factor. Take a snapshot.
    size_t perfect = 0;
    auto update_perfect = [&](Key k) {
      perfect += GetHashtableDebugNumProbes(t, k) == 0;
    };
    for (const auto& k : keys) update_perfect(k);

    std::vector<double> perfect_ratios;
    // Keep going until we hit max load factor.
    while (t.load_factor() < .6) {
      perfect_ratios.push_back(1.0 * perfect / t.size());
      update_perfect(add(t));
    }
    while (t.load_factor() > .5) {
      perfect_ratios.push_back(1.0 * perfect / t.size());
      update_perfect(add(t));
    }

    results[0] += perfect_ratios.front();
    results[1] += perfect_ratios[perfect_ratios.size() / 2];
    results[2] += perfect_ratios.back();
  }

  results[0] /= kNumTrials;
  results[1] /= kNumTrials;
  results[2] /= kNumTrials;

  return results;
}

std::vector<std::pair<double, double>> StringTablePefectRatios() {
  constexpr bool kRandomizesInserts =
#if NDEBUG
      false;
#else   // NDEBUG
      true;
#endif  // NDEBUG

  // The effective load factor is larger in non-opt mode because we insert
  // elements out of order.
  switch (container_internal::Group::kWidth) {
    case 8:
      if (kRandomizesInserts) {
        return {{0.986, 0.02}, {0.95, 0.02}, {0.89, 0.02}};
      } else {
        return {{0.995, 0.01}, {0.97, 0.01}, {0.89, 0.02}};
      }
    case 16:
      if (kRandomizesInserts) {
        return {{0.973, 0.01}, {0.965, 0.01}, {0.92, 0.02}};
      } else {
        return {{0.995, 0.005}, {0.99, 0.005}, {0.94, 0.01}};
      }
  }
  ABSL_RAW_LOG(FATAL, "%s", "Unknown Group width");
  return {};
}

// This is almost a change detector, but it allows us to know how we are
// affecting the probe distribution.
TEST(Table, EffectiveLoadFactorStrings) {
  std::vector<double> perfect_ratios =
      CollectPerfectRatios(StringTable(), [](StringTable& t) {
        return t.emplace(std::to_string(t.size()), "").first->first;
      });

  auto ratios = StringTablePefectRatios();
  if (ratios.empty()) return;
  EXPECT_THAT(perfect_ratios,
              ElementsAre(DoubleNear(ratios[0].first, ratios[0].second),
                          DoubleNear(ratios[1].first, ratios[1].second),
                          DoubleNear(ratios[2].first, ratios[2].second)));
}

std::vector<std::pair<double, double>> IntTablePefectRatios() {
  constexpr bool kRandomizesInserts =
#ifdef NDEBUG
      false;
#else   // NDEBUG
      true;
#endif  // NDEBUG

  // The effective load factor is larger in non-opt mode because we insert
  // elements out of order.
  switch (container_internal::Group::kWidth) {
    case 8:
      if (kRandomizesInserts) {
        return {{0.99, 0.02}, {0.985, 0.02}, {0.95, 0.05}};
      } else {
        return {{0.99, 0.01}, {0.99, 0.01}, {0.95, 0.02}};
      }
    case 16:
      if (kRandomizesInserts) {
        return {{0.98, 0.02}, {0.978, 0.02}, {0.96, 0.02}};
      } else {
        return {{0.998, 0.003}, {0.995, 0.01}, {0.975, 0.02}};
      }
  }
  ABSL_RAW_LOG(FATAL, "%s", "Unknown Group width");
  return {};
}

// This is almost a change detector, but it allows us to know how we are
// affecting the probe distribution.
TEST(Table, EffectiveLoadFactorInts) {
  std::vector<double> perfect_ratios = CollectPerfectRatios(
      IntTable(), [](IntTable& t) { return *t.emplace(t.size()).first; });

  auto ratios = IntTablePefectRatios();
  if (ratios.empty()) return;

  EXPECT_THAT(perfect_ratios,
              ElementsAre(DoubleNear(ratios[0].first, ratios[0].second),
                          DoubleNear(ratios[1].first, ratios[1].second),
                          DoubleNear(ratios[2].first, ratios[2].second)));
}

// Confirm that we assert if we try to erase() end().
TEST(TableDeathTest, EraseOfEndAsserts) {
  // Use an assert with side-effects to figure out if they are actually enabled.
  bool assert_enabled = false;
  assert([&]() {
    assert_enabled = true;
    return true;
  }());
  if (!assert_enabled) return;

  IntTable t;
  // Extra simple "regexp" as regexp support is highly varied across platforms.
  constexpr char kDeathMsg[] = "it != end";
  EXPECT_DEATH_IF_SUPPORTED(t.erase(t.end()), kDeathMsg);
}

#ifdef ADDRESS_SANITIZER
TEST(Sanitizer, PoisoningUnused) {
  IntTable t;
  // Insert something to force an allocation.
  int64_t& v1 = *t.insert(0).first;

  // Make sure there is something to test.
  ASSERT_GT(t.capacity(), 1);

  int64_t* slots = RawHashSetTestOnlyAccess::GetSlots(t);
  for (size_t i = 0; i < t.capacity(); ++i) {
    EXPECT_EQ(slots + i != &v1, __asan_address_is_poisoned(slots + i));
  }
}

TEST(Sanitizer, PoisoningOnErase) {
  IntTable t;
  int64_t& v = *t.insert(0).first;

  EXPECT_FALSE(__asan_address_is_poisoned(&v));
  t.erase(0);
  EXPECT_TRUE(__asan_address_is_poisoned(&v));
}
#endif  // ADDRESS_SANITIZER

}  // namespace
}  // namespace container_internal
}  // namespace absl
