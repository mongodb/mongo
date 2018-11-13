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

#include "absl/container/flat_hash_map.h"

#include "absl/container/internal/hash_generator_testing.h"
#include "absl/container/internal/unordered_map_constructor_test.h"
#include "absl/container/internal/unordered_map_lookup_test.h"
#include "absl/container/internal/unordered_map_modifiers_test.h"
#include "absl/types/any.h"

namespace absl {
namespace container_internal {
namespace {
using ::absl::container_internal::hash_internal::Enum;
using ::absl::container_internal::hash_internal::EnumClass;
using ::testing::_;
using ::testing::Pair;
using ::testing::UnorderedElementsAre;

template <class K, class V>
using Map =
    flat_hash_map<K, V, StatefulTestingHash, StatefulTestingEqual, Alloc<>>;

static_assert(!std::is_standard_layout<NonStandardLayout>(), "");

using MapTypes =
    ::testing::Types<Map<int, int>, Map<std::string, int>, Map<Enum, std::string>,
                     Map<EnumClass, int>, Map<int, NonStandardLayout>,
                     Map<NonStandardLayout, int>>;

INSTANTIATE_TYPED_TEST_CASE_P(FlatHashMap, ConstructorTest, MapTypes);
INSTANTIATE_TYPED_TEST_CASE_P(FlatHashMap, LookupTest, MapTypes);
INSTANTIATE_TYPED_TEST_CASE_P(FlatHashMap, ModifiersTest, MapTypes);

TEST(FlatHashMap, StandardLayout) {
  struct Int {
    explicit Int(size_t value) : value(value) {}
    Int() : value(0) { ADD_FAILURE(); }
    Int(const Int& other) : value(other.value) { ADD_FAILURE(); }
    Int(Int&&) = default;
    bool operator==(const Int& other) const { return value == other.value; }
    size_t value;
  };
  static_assert(std::is_standard_layout<Int>(), "");

  struct Hash {
    size_t operator()(const Int& obj) const { return obj.value; }
  };

  // Verify that neither the key nor the value get default-constructed or
  // copy-constructed.
  {
    flat_hash_map<Int, Int, Hash> m;
    m.try_emplace(Int(1), Int(2));
    m.try_emplace(Int(3), Int(4));
    m.erase(Int(1));
    m.rehash(2 * m.bucket_count());
  }
  {
    flat_hash_map<Int, Int, Hash> m;
    m.try_emplace(Int(1), Int(2));
    m.try_emplace(Int(3), Int(4));
    m.erase(Int(1));
    m.clear();
  }
}

// gcc becomes unhappy if this is inside the method, so pull it out here.
struct balast {};

TEST(FlatHashMap, IteratesMsan) {
  // Because SwissTable randomizes on pointer addresses, we keep old tables
  // around to ensure we don't reuse old memory.
  std::vector<absl::flat_hash_map<int, balast>> garbage;
  for (int i = 0; i < 100; ++i) {
    absl::flat_hash_map<int, balast> t;
    for (int j = 0; j < 100; ++j) {
      t[j];
      for (const auto& p : t) EXPECT_THAT(p, Pair(_, _));
    }
    garbage.push_back(std::move(t));
  }
}

// Demonstration of the "Lazy Key" pattern.  This uses heterogeneous insert to
// avoid creating expensive key elements when the item is already present in the
// map.
struct LazyInt {
  explicit LazyInt(size_t value, int* tracker)
      : value(value), tracker(tracker) {}

  explicit operator size_t() const {
    ++*tracker;
    return value;
  }

  size_t value;
  int* tracker;
};

struct Hash {
  using is_transparent = void;
  int* tracker;
  size_t operator()(size_t obj) const {
    ++*tracker;
    return obj;
  }
  size_t operator()(const LazyInt& obj) const {
    ++*tracker;
    return obj.value;
  }
};

struct Eq {
  using is_transparent = void;
  bool operator()(size_t lhs, size_t rhs) const {
    return lhs == rhs;
  }
  bool operator()(size_t lhs, const LazyInt& rhs) const {
    return lhs == rhs.value;
  }
};

TEST(FlatHashMap, LazyKeyPattern) {
  // hashes are only guaranteed in opt mode, we use assertions to track internal
  // state that can cause extra calls to hash.
  int conversions = 0;
  int hashes = 0;
  flat_hash_map<size_t, size_t, Hash, Eq> m(0, Hash{&hashes});

  m[LazyInt(1, &conversions)] = 1;
  EXPECT_THAT(m, UnorderedElementsAre(Pair(1, 1)));
  EXPECT_EQ(conversions, 1);
#ifdef NDEBUG
  EXPECT_EQ(hashes, 1);
#endif

  m[LazyInt(1, &conversions)] = 2;
  EXPECT_THAT(m, UnorderedElementsAre(Pair(1, 2)));
  EXPECT_EQ(conversions, 1);
#ifdef NDEBUG
  EXPECT_EQ(hashes, 2);
#endif

  m.try_emplace(LazyInt(2, &conversions), 3);
  EXPECT_THAT(m, UnorderedElementsAre(Pair(1, 2), Pair(2, 3)));
  EXPECT_EQ(conversions, 2);
#ifdef NDEBUG
  EXPECT_EQ(hashes, 3);
#endif

  m.try_emplace(LazyInt(2, &conversions), 4);
  EXPECT_THAT(m, UnorderedElementsAre(Pair(1, 2), Pair(2, 3)));
  EXPECT_EQ(conversions, 2);
#ifdef NDEBUG
  EXPECT_EQ(hashes, 4);
#endif
}

TEST(FlatHashMap, BitfieldArgument) {
  union {
    int n : 1;
  };
  n = 0;
  flat_hash_map<int, int> m;
  m.erase(n);
  m.count(n);
  m.prefetch(n);
  m.find(n);
  m.contains(n);
  m.equal_range(n);
  m.insert_or_assign(n, n);
  m.insert_or_assign(m.end(), n, n);
  m.try_emplace(n);
  m.try_emplace(m.end(), n);
  m.at(n);
  m[n];
}

TEST(FlatHashMap, MergeExtractInsert) {
  // We can't test mutable keys, or non-copyable keys with flat_hash_map.
  // Test that the nodes have the proper API.
  absl::flat_hash_map<int, int> m = {{1, 7}, {2, 9}};
  auto node = m.extract(1);
  EXPECT_TRUE(node);
  EXPECT_EQ(node.key(), 1);
  EXPECT_EQ(node.mapped(), 7);
  EXPECT_THAT(m, UnorderedElementsAre(Pair(2, 9)));

  node.mapped() = 17;
  m.insert(std::move(node));
  EXPECT_THAT(m, UnorderedElementsAre(Pair(1, 17), Pair(2, 9)));
}
#if !defined(__ANDROID__) && !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
TEST(FlatHashMap, Any) {
  absl::flat_hash_map<int, absl::any> m;
  m.emplace(1, 7);
  auto it = m.find(1);
  ASSERT_NE(it, m.end());
  EXPECT_EQ(7, absl::any_cast<int>(it->second));

  m.emplace(std::piecewise_construct, std::make_tuple(2), std::make_tuple(8));
  it = m.find(2);
  ASSERT_NE(it, m.end());
  EXPECT_EQ(8, absl::any_cast<int>(it->second));

  m.emplace(std::piecewise_construct, std::make_tuple(3),
            std::make_tuple(absl::any(9)));
  it = m.find(3);
  ASSERT_NE(it, m.end());
  EXPECT_EQ(9, absl::any_cast<int>(it->second));

  struct H {
    size_t operator()(const absl::any&) const { return 0; }
  };
  struct E {
    bool operator()(const absl::any&, const absl::any&) const { return true; }
  };
  absl::flat_hash_map<absl::any, int, H, E> m2;
  m2.emplace(1, 7);
  auto it2 = m2.find(1);
  ASSERT_NE(it2, m2.end());
  EXPECT_EQ(7, it2->second);
}
#endif  // __ANDROID__

}  // namespace
}  // namespace container_internal
}  // namespace absl
