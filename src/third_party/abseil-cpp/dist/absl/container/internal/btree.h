// Copyright 2018 The Abseil Authors.
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

// A btree implementation of the STL set and map interfaces. A btree is smaller
// and generally also faster than STL set/map (refer to the benchmarks below).
// The red-black tree implementation of STL set/map has an overhead of 3
// pointers (left, right and parent) plus the node color information for each
// stored value. So a set<int32_t> consumes 40 bytes for each value stored in
// 64-bit mode. This btree implementation stores multiple values on fixed
// size nodes (usually 256 bytes) and doesn't store child pointers for leaf
// nodes. The result is that a btree_set<int32_t> may use much less memory per
// stored value. For the random insertion benchmark in btree_bench.cc, a
// btree_set<int32_t> with node-size of 256 uses 5.1 bytes per stored value.
//
// The packing of multiple values on to each node of a btree has another effect
// besides better space utilization: better cache locality due to fewer cache
// lines being accessed. Better cache locality translates into faster
// operations.
//
// CAVEATS
//
// Insertions and deletions on a btree can cause splitting, merging or
// rebalancing of btree nodes. And even without these operations, insertions
// and deletions on a btree will move values around within a node. In both
// cases, the result is that insertions and deletions can invalidate iterators
// pointing to values other than the one being inserted/deleted. Therefore, this
// container does not provide pointer stability. This is notably different from
// STL set/map which takes care to not invalidate iterators on insert/erase
// except, of course, for iterators pointing to the value being erased.  A
// partial workaround when erasing is available: erase() returns an iterator
// pointing to the item just after the one that was erased (or end() if none
// exists).

#ifndef ABSL_CONTAINER_INTERNAL_BTREE_H_
#define ABSL_CONTAINER_INTERNAL_BTREE_H_

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

#include "absl/base/macros.h"
#include "absl/container/internal/common.h"
#include "absl/container/internal/compressed_tuple.h"
#include "absl/container/internal/container_memory.h"
#include "absl/container/internal/layout.h"
#include "absl/memory/memory.h"
#include "absl/meta/type_traits.h"
#include "absl/strings/cord.h"
#include "absl/strings/string_view.h"
#include "absl/types/compare.h"
#include "absl/utility/utility.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace container_internal {

// A helper class that indicates if the Compare parameter is a key-compare-to
// comparator.
template <typename Compare, typename T>
using btree_is_key_compare_to =
    std::is_convertible<absl::result_of_t<Compare(const T &, const T &)>,
                        absl::weak_ordering>;

struct StringBtreeDefaultLess {
  using is_transparent = void;

  StringBtreeDefaultLess() = default;

  // Compatibility constructor.
  StringBtreeDefaultLess(std::less<std::string>) {}  // NOLINT
  StringBtreeDefaultLess(std::less<absl::string_view>) {}  // NOLINT

  // Allow converting to std::less for use in key_comp()/value_comp().
  explicit operator std::less<std::string>() const { return {}; }
  explicit operator std::less<absl::string_view>() const { return {}; }
  explicit operator std::less<absl::Cord>() const { return {}; }

  absl::weak_ordering operator()(absl::string_view lhs,
                                 absl::string_view rhs) const {
    return compare_internal::compare_result_as_ordering(lhs.compare(rhs));
  }
  StringBtreeDefaultLess(std::less<absl::Cord>) {}  // NOLINT
  absl::weak_ordering operator()(const absl::Cord &lhs,
                                 const absl::Cord &rhs) const {
    return compare_internal::compare_result_as_ordering(lhs.Compare(rhs));
  }
  absl::weak_ordering operator()(const absl::Cord &lhs,
                                 absl::string_view rhs) const {
    return compare_internal::compare_result_as_ordering(lhs.Compare(rhs));
  }
  absl::weak_ordering operator()(absl::string_view lhs,
                                 const absl::Cord &rhs) const {
    return compare_internal::compare_result_as_ordering(-rhs.Compare(lhs));
  }
};

struct StringBtreeDefaultGreater {
  using is_transparent = void;

  StringBtreeDefaultGreater() = default;

  StringBtreeDefaultGreater(std::greater<std::string>) {}  // NOLINT
  StringBtreeDefaultGreater(std::greater<absl::string_view>) {}  // NOLINT

  // Allow converting to std::greater for use in key_comp()/value_comp().
  explicit operator std::greater<std::string>() const { return {}; }
  explicit operator std::greater<absl::string_view>() const { return {}; }
  explicit operator std::greater<absl::Cord>() const { return {}; }

  absl::weak_ordering operator()(absl::string_view lhs,
                                 absl::string_view rhs) const {
    return compare_internal::compare_result_as_ordering(rhs.compare(lhs));
  }
  StringBtreeDefaultGreater(std::greater<absl::Cord>) {}  // NOLINT
  absl::weak_ordering operator()(const absl::Cord &lhs,
                                 const absl::Cord &rhs) const {
    return compare_internal::compare_result_as_ordering(rhs.Compare(lhs));
  }
  absl::weak_ordering operator()(const absl::Cord &lhs,
                                 absl::string_view rhs) const {
    return compare_internal::compare_result_as_ordering(-lhs.Compare(rhs));
  }
  absl::weak_ordering operator()(absl::string_view lhs,
                                 const absl::Cord &rhs) const {
    return compare_internal::compare_result_as_ordering(rhs.Compare(lhs));
  }
};

// A helper class to convert a boolean comparison into a three-way "compare-to"
// comparison that returns an `absl::weak_ordering`. This helper
// class is specialized for less<std::string>, greater<std::string>,
// less<string_view>, greater<string_view>, less<absl::Cord>, and
// greater<absl::Cord>.
//
// key_compare_to_adapter is provided so that btree users
// automatically get the more efficient compare-to code when using common
// Abseil string types with common comparison functors.
// These string-like specializations also turn on heterogeneous lookup by
// default.
template <typename Compare>
struct key_compare_to_adapter {
  using type = Compare;
};

template <>
struct key_compare_to_adapter<std::less<std::string>> {
  using type = StringBtreeDefaultLess;
};

template <>
struct key_compare_to_adapter<std::greater<std::string>> {
  using type = StringBtreeDefaultGreater;
};

template <>
struct key_compare_to_adapter<std::less<absl::string_view>> {
  using type = StringBtreeDefaultLess;
};

template <>
struct key_compare_to_adapter<std::greater<absl::string_view>> {
  using type = StringBtreeDefaultGreater;
};

template <>
struct key_compare_to_adapter<std::less<absl::Cord>> {
  using type = StringBtreeDefaultLess;
};

template <>
struct key_compare_to_adapter<std::greater<absl::Cord>> {
  using type = StringBtreeDefaultGreater;
};

// Detects an 'absl_btree_prefer_linear_node_search' member. This is
// a protocol used as an opt-in or opt-out of linear search.
//
//  For example, this would be useful for key types that wrap an integer
//  and define their own cheap operator<(). For example:
//
//   class K {
//    public:
//     using absl_btree_prefer_linear_node_search = std::true_type;
//     ...
//    private:
//     friend bool operator<(K a, K b) { return a.k_ < b.k_; }
//     int k_;
//   };
//
//   btree_map<K, V> m;  // Uses linear search
//
// If T has the preference tag, then it has a preference.
// Btree will use the tag's truth value.
template <typename T, typename = void>
struct has_linear_node_search_preference : std::false_type {};
template <typename T, typename = void>
struct prefers_linear_node_search : std::false_type {};
template <typename T>
struct has_linear_node_search_preference<
    T, absl::void_t<typename T::absl_btree_prefer_linear_node_search>>
    : std::true_type {};
template <typename T>
struct prefers_linear_node_search<
    T, absl::void_t<typename T::absl_btree_prefer_linear_node_search>>
    : T::absl_btree_prefer_linear_node_search {};

template <typename Key, typename Compare, typename Alloc, int TargetNodeSize,
          bool Multi, typename SlotPolicy>
struct common_params {
  using original_key_compare = Compare;

  // If Compare is a common comparator for a string-like type, then we adapt it
  // to use heterogeneous lookup and to be a key-compare-to comparator.
  using key_compare = typename key_compare_to_adapter<Compare>::type;
  // A type which indicates if we have a key-compare-to functor or a plain old
  // key-compare functor.
  using is_key_compare_to = btree_is_key_compare_to<key_compare, Key>;

  using allocator_type = Alloc;
  using key_type = Key;
  using size_type = std::make_signed<size_t>::type;
  using difference_type = ptrdiff_t;

  using slot_policy = SlotPolicy;
  using slot_type = typename slot_policy::slot_type;
  using value_type = typename slot_policy::value_type;
  using init_type = typename slot_policy::mutable_value_type;
  using pointer = value_type *;
  using const_pointer = const value_type *;
  using reference = value_type &;
  using const_reference = const value_type &;

  // For the given lookup key type, returns whether we can have multiple
  // equivalent keys in the btree. If this is a multi-container, then we can.
  // Otherwise, we can have multiple equivalent keys only if all of the
  // following conditions are met:
  // - The comparator is transparent.
  // - The lookup key type is not the same as key_type.
  // - The comparator is not a StringBtreeDefault{Less,Greater} comparator
  //   that we know has the same equivalence classes for all lookup types.
  template <typename LookupKey>
  constexpr static bool can_have_multiple_equivalent_keys() {
    return Multi ||
           (IsTransparent<key_compare>::value &&
            !std::is_same<LookupKey, Key>::value &&
            !std::is_same<key_compare, StringBtreeDefaultLess>::value &&
            !std::is_same<key_compare, StringBtreeDefaultGreater>::value);
  }

  enum {
    kTargetNodeSize = TargetNodeSize,

    // Upper bound for the available space for values. This is largest for leaf
    // nodes, which have overhead of at least a pointer + 4 bytes (for storing
    // 3 field_types and an enum).
    kNodeValueSpace =
        TargetNodeSize - /*minimum overhead=*/(sizeof(void *) + 4),
  };

  // This is an integral type large enough to hold as many
  // ValueSize-values as will fit a node of TargetNodeSize bytes.
  using node_count_type =
      absl::conditional_t<(kNodeValueSpace / sizeof(value_type) >
                           (std::numeric_limits<uint8_t>::max)()),
                          uint16_t, uint8_t>;  // NOLINT

  // The following methods are necessary for passing this struct as PolicyTraits
  // for node_handle and/or are used within btree.
  static value_type &element(slot_type *slot) {
    return slot_policy::element(slot);
  }
  static const value_type &element(const slot_type *slot) {
    return slot_policy::element(slot);
  }
  template <class... Args>
  static void construct(Alloc *alloc, slot_type *slot, Args &&... args) {
    slot_policy::construct(alloc, slot, std::forward<Args>(args)...);
  }
  static void construct(Alloc *alloc, slot_type *slot, slot_type *other) {
    slot_policy::construct(alloc, slot, other);
  }
  static void destroy(Alloc *alloc, slot_type *slot) {
    slot_policy::destroy(alloc, slot);
  }
  static void transfer(Alloc *alloc, slot_type *new_slot, slot_type *old_slot) {
    construct(alloc, new_slot, old_slot);
    destroy(alloc, old_slot);
  }
  static void swap(Alloc *alloc, slot_type *a, slot_type *b) {
    slot_policy::swap(alloc, a, b);
  }
  static void move(Alloc *alloc, slot_type *src, slot_type *dest) {
    slot_policy::move(alloc, src, dest);
  }
};

// A parameters structure for holding the type parameters for a btree_map.
// Compare and Alloc should be nothrow copy-constructible.
template <typename Key, typename Data, typename Compare, typename Alloc,
          int TargetNodeSize, bool Multi>
struct map_params : common_params<Key, Compare, Alloc, TargetNodeSize, Multi,
                                  map_slot_policy<Key, Data>> {
  using super_type = typename map_params::common_params;
  using mapped_type = Data;
  // This type allows us to move keys when it is safe to do so. It is safe
  // for maps in which value_type and mutable_value_type are layout compatible.
  using slot_policy = typename super_type::slot_policy;
  using slot_type = typename super_type::slot_type;
  using value_type = typename super_type::value_type;
  using init_type = typename super_type::init_type;

  using original_key_compare = typename super_type::original_key_compare;
  // Reference: https://en.cppreference.com/w/cpp/container/map/value_compare
  class value_compare {
    template <typename Params>
    friend class btree;

   protected:
    explicit value_compare(original_key_compare c) : comp(std::move(c)) {}

    original_key_compare comp;  // NOLINT

   public:
    auto operator()(const value_type &lhs, const value_type &rhs) const
        -> decltype(comp(lhs.first, rhs.first)) {
      return comp(lhs.first, rhs.first);
    }
  };
  using is_map_container = std::true_type;

  template <typename V>
  static auto key(const V &value) -> decltype(value.first) {
    return value.first;
  }
  static const Key &key(const slot_type *s) { return slot_policy::key(s); }
  static const Key &key(slot_type *s) { return slot_policy::key(s); }
  // For use in node handle.
  static auto mutable_key(slot_type *s)
      -> decltype(slot_policy::mutable_key(s)) {
    return slot_policy::mutable_key(s);
  }
  static mapped_type &value(value_type *value) { return value->second; }
};

// This type implements the necessary functions from the
// absl::container_internal::slot_type interface.
template <typename Key>
struct set_slot_policy {
  using slot_type = Key;
  using value_type = Key;
  using mutable_value_type = Key;

  static value_type &element(slot_type *slot) { return *slot; }
  static const value_type &element(const slot_type *slot) { return *slot; }

  template <typename Alloc, class... Args>
  static void construct(Alloc *alloc, slot_type *slot, Args &&... args) {
    absl::allocator_traits<Alloc>::construct(*alloc, slot,
                                             std::forward<Args>(args)...);
  }

  template <typename Alloc>
  static void construct(Alloc *alloc, slot_type *slot, slot_type *other) {
    absl::allocator_traits<Alloc>::construct(*alloc, slot, std::move(*other));
  }

  template <typename Alloc>
  static void destroy(Alloc *alloc, slot_type *slot) {
    absl::allocator_traits<Alloc>::destroy(*alloc, slot);
  }

  template <typename Alloc>
  static void swap(Alloc * /*alloc*/, slot_type *a, slot_type *b) {
    using std::swap;
    swap(*a, *b);
  }

  template <typename Alloc>
  static void move(Alloc * /*alloc*/, slot_type *src, slot_type *dest) {
    *dest = std::move(*src);
  }
};

// A parameters structure for holding the type parameters for a btree_set.
// Compare and Alloc should be nothrow copy-constructible.
template <typename Key, typename Compare, typename Alloc, int TargetNodeSize,
          bool Multi>
struct set_params : common_params<Key, Compare, Alloc, TargetNodeSize, Multi,
                                  set_slot_policy<Key>> {
  using value_type = Key;
  using slot_type = typename set_params::common_params::slot_type;
  using value_compare =
      typename set_params::common_params::original_key_compare;
  using is_map_container = std::false_type;

  template <typename V>
  static const V &key(const V &value) { return value; }
  static const Key &key(const slot_type *slot) { return *slot; }
  static const Key &key(slot_type *slot) { return *slot; }
};

// An adapter class that converts a lower-bound compare into an upper-bound
// compare. Note: there is no need to make a version of this adapter specialized
// for key-compare-to functors because the upper-bound (the first value greater
// than the input) is never an exact match.
template <typename Compare>
struct upper_bound_adapter {
  explicit upper_bound_adapter(const Compare &c) : comp(c) {}
  template <typename K1, typename K2>
  bool operator()(const K1 &a, const K2 &b) const {
    // Returns true when a is not greater than b.
    return !compare_internal::compare_result_as_less_than(comp(b, a));
  }

 private:
  Compare comp;
};

enum class MatchKind : uint8_t { kEq, kNe };

template <typename V, bool IsCompareTo>
struct SearchResult {
  V value;
  MatchKind match;

  static constexpr bool HasMatch() { return true; }
  bool IsEq() const { return match == MatchKind::kEq; }
};

// When we don't use CompareTo, `match` is not present.
// This ensures that callers can't use it accidentally when it provides no
// useful information.
template <typename V>
struct SearchResult<V, false> {
  SearchResult() {}
  explicit SearchResult(V value) : value(value) {}
  SearchResult(V value, MatchKind /*match*/) : value(value) {}

  V value;

  static constexpr bool HasMatch() { return false; }
  static constexpr bool IsEq() { return false; }
};

// A node in the btree holding. The same node type is used for both internal
// and leaf nodes in the btree, though the nodes are allocated in such a way
// that the children array is only valid in internal nodes.
template <typename Params>
class btree_node {
  using is_key_compare_to = typename Params::is_key_compare_to;
  using field_type = typename Params::node_count_type;
  using allocator_type = typename Params::allocator_type;
  using slot_type = typename Params::slot_type;

 public:
  using params_type = Params;
  using key_type = typename Params::key_type;
  using value_type = typename Params::value_type;
  using pointer = typename Params::pointer;
  using const_pointer = typename Params::const_pointer;
  using reference = typename Params::reference;
  using const_reference = typename Params::const_reference;
  using key_compare = typename Params::key_compare;
  using size_type = typename Params::size_type;
  using difference_type = typename Params::difference_type;

  // Btree decides whether to use linear node search as follows:
  //   - If the comparator expresses a preference, use that.
  //   - If the key expresses a preference, use that.
  //   - If the key is arithmetic and the comparator is std::less or
  //     std::greater, choose linear.
  //   - Otherwise, choose binary.
  // TODO(ezb): Might make sense to add condition(s) based on node-size.
  using use_linear_search = std::integral_constant<
      bool,
      has_linear_node_search_preference<key_compare>::value
          ? prefers_linear_node_search<key_compare>::value
          : has_linear_node_search_preference<key_type>::value
                ? prefers_linear_node_search<key_type>::value
                : std::is_arithmetic<key_type>::value &&
                      (std::is_same<std::less<key_type>, key_compare>::value ||
                       std::is_same<std::greater<key_type>,
                                    key_compare>::value)>;

  // This class is organized by absl::container_internal::Layout as if it had
  // the following structure:
  //   // A pointer to the node's parent.
  //   btree_node *parent;
  //
  //   // The position of the node in the node's parent.
  //   field_type position;
  //   // The index of the first populated value in `values`.
  //   // TODO(ezb): right now, `start` is always 0. Update insertion/merge
  //   // logic to allow for floating storage within nodes.
  //   field_type start;
  //   // The index after the last populated value in `values`. Currently, this
  //   // is the same as the count of values.
  //   field_type finish;
  //   // The maximum number of values the node can hold. This is an integer in
  //   // [1, kNodeSlots] for root leaf nodes, kNodeSlots for non-root leaf
  //   // nodes, and kInternalNodeMaxCount (as a sentinel value) for internal
  //   // nodes (even though there are still kNodeSlots values in the node).
  //   // TODO(ezb): make max_count use only 4 bits and record log2(capacity)
  //   // to free extra bits for is_root, etc.
  //   field_type max_count;
  //
  //   // The array of values. The capacity is `max_count` for leaf nodes and
  //   // kNodeSlots for internal nodes. Only the values in
  //   // [start, finish) have been initialized and are valid.
  //   slot_type values[max_count];
  //
  //   // The array of child pointers. The keys in children[i] are all less
  //   // than key(i). The keys in children[i + 1] are all greater than key(i).
  //   // There are 0 children for leaf nodes and kNodeSlots + 1 children for
  //   // internal nodes.
  //   btree_node *children[kNodeSlots + 1];
  //
  // This class is only constructed by EmptyNodeType. Normally, pointers to the
  // layout above are allocated, cast to btree_node*, and de-allocated within
  // the btree implementation.
  ~btree_node() = default;
  btree_node(btree_node const &) = delete;
  btree_node &operator=(btree_node const &) = delete;

  // Public for EmptyNodeType.
  constexpr static size_type Alignment() {
    static_assert(LeafLayout(1).Alignment() == InternalLayout().Alignment(),
                  "Alignment of all nodes must be equal.");
    return InternalLayout().Alignment();
  }

 protected:
  btree_node() = default;

 private:
  using layout_type = absl::container_internal::Layout<btree_node *, field_type,
                                                       slot_type, btree_node *>;
  constexpr static size_type SizeWithNSlots(size_type n) {
    return layout_type(/*parent*/ 1,
                       /*position, start, finish, max_count*/ 4,
                       /*slots*/ n,
                       /*children*/ 0)
        .AllocSize();
  }
  // A lower bound for the overhead of fields other than values in a leaf node.
  constexpr static size_type MinimumOverhead() {
    return SizeWithNSlots(1) - sizeof(value_type);
  }

  // Compute how many values we can fit onto a leaf node taking into account
  // padding.
  constexpr static size_type NodeTargetSlots(const int begin, const int end) {
    return begin == end ? begin
                        : SizeWithNSlots((begin + end) / 2 + 1) >
                                  params_type::kTargetNodeSize
                              ? NodeTargetSlots(begin, (begin + end) / 2)
                              : NodeTargetSlots((begin + end) / 2 + 1, end);
  }

  enum {
    kTargetNodeSize = params_type::kTargetNodeSize,
    kNodeTargetSlots = NodeTargetSlots(0, params_type::kTargetNodeSize),

    // We need a minimum of 3 slots per internal node in order to perform
    // splitting (1 value for the two nodes involved in the split and 1 value
    // propagated to the parent as the delimiter for the split). For performance
    // reasons, we don't allow 3 slots-per-node due to bad worst case occupancy
    // of 1/3 (for a node, not a b-tree).
    kMinNodeSlots = 4,

    kNodeSlots =
        kNodeTargetSlots >= kMinNodeSlots ? kNodeTargetSlots : kMinNodeSlots,

    // The node is internal (i.e. is not a leaf node) if and only if `max_count`
    // has this value.
    kInternalNodeMaxCount = 0,
  };

  // Leaves can have less than kNodeSlots values.
  constexpr static layout_type LeafLayout(const int slot_count = kNodeSlots) {
    return layout_type(/*parent*/ 1,
                       /*position, start, finish, max_count*/ 4,
                       /*slots*/ slot_count,
                       /*children*/ 0);
  }
  constexpr static layout_type InternalLayout() {
    return layout_type(/*parent*/ 1,
                       /*position, start, finish, max_count*/ 4,
                       /*slots*/ kNodeSlots,
                       /*children*/ kNodeSlots + 1);
  }
  constexpr static size_type LeafSize(const int slot_count = kNodeSlots) {
    return LeafLayout(slot_count).AllocSize();
  }
  constexpr static size_type InternalSize() {
    return InternalLayout().AllocSize();
  }

  // N is the index of the type in the Layout definition.
  // ElementType<N> is the Nth type in the Layout definition.
  template <size_type N>
  inline typename layout_type::template ElementType<N> *GetField() {
    // We assert that we don't read from values that aren't there.
    assert(N < 3 || !leaf());
    return InternalLayout().template Pointer<N>(reinterpret_cast<char *>(this));
  }
  template <size_type N>
  inline const typename layout_type::template ElementType<N> *GetField() const {
    assert(N < 3 || !leaf());
    return InternalLayout().template Pointer<N>(
        reinterpret_cast<const char *>(this));
  }
  void set_parent(btree_node *p) { *GetField<0>() = p; }
  field_type &mutable_finish() { return GetField<1>()[2]; }
  slot_type *slot(int i) { return &GetField<2>()[i]; }
  slot_type *start_slot() { return slot(start()); }
  slot_type *finish_slot() { return slot(finish()); }
  const slot_type *slot(int i) const { return &GetField<2>()[i]; }
  void set_position(field_type v) { GetField<1>()[0] = v; }
  void set_start(field_type v) { GetField<1>()[1] = v; }
  void set_finish(field_type v) { GetField<1>()[2] = v; }
  // This method is only called by the node init methods.
  void set_max_count(field_type v) { GetField<1>()[3] = v; }

 public:
  // Whether this is a leaf node or not. This value doesn't change after the
  // node is created.
  bool leaf() const { return GetField<1>()[3] != kInternalNodeMaxCount; }

  // Getter for the position of this node in its parent.
  field_type position() const { return GetField<1>()[0]; }

  // Getter for the offset of the first value in the `values` array.
  field_type start() const {
    // TODO(ezb): when floating storage is implemented, return GetField<1>()[1];
    assert(GetField<1>()[1] == 0);
    return 0;
  }

  // Getter for the offset after the last value in the `values` array.
  field_type finish() const { return GetField<1>()[2]; }

  // Getters for the number of values stored in this node.
  field_type count() const {
    assert(finish() >= start());
    return finish() - start();
  }
  field_type max_count() const {
    // Internal nodes have max_count==kInternalNodeMaxCount.
    // Leaf nodes have max_count in [1, kNodeSlots].
    const field_type max_count = GetField<1>()[3];
    return max_count == field_type{kInternalNodeMaxCount}
               ? field_type{kNodeSlots}
               : max_count;
  }

  // Getter for the parent of this node.
  btree_node *parent() const { return *GetField<0>(); }
  // Getter for whether the node is the root of the tree. The parent of the
  // root of the tree is the leftmost node in the tree which is guaranteed to
  // be a leaf.
  bool is_root() const { return parent()->leaf(); }
  void make_root() {
    assert(parent()->is_root());
    set_parent(parent()->parent());
  }

  // Getters for the key/value at position i in the node.
  const key_type &key(int i) const { return params_type::key(slot(i)); }
  reference value(int i) { return params_type::element(slot(i)); }
  const_reference value(int i) const { return params_type::element(slot(i)); }

  // Getters/setter for the child at position i in the node.
  btree_node *child(int i) const { return GetField<3>()[i]; }
  btree_node *start_child() const { return child(start()); }
  btree_node *&mutable_child(int i) { return GetField<3>()[i]; }
  void clear_child(int i) {
    absl::container_internal::SanitizerPoisonObject(&mutable_child(i));
  }
  void set_child(int i, btree_node *c) {
    absl::container_internal::SanitizerUnpoisonObject(&mutable_child(i));
    mutable_child(i) = c;
    c->set_position(i);
  }
  void init_child(int i, btree_node *c) {
    set_child(i, c);
    c->set_parent(this);
  }

  // Returns the position of the first value whose key is not less than k.
  template <typename K>
  SearchResult<int, is_key_compare_to::value> lower_bound(
      const K &k, const key_compare &comp) const {
    return use_linear_search::value ? linear_search(k, comp)
                                    : binary_search(k, comp);
  }
  // Returns the position of the first value whose key is greater than k.
  template <typename K>
  int upper_bound(const K &k, const key_compare &comp) const {
    auto upper_compare = upper_bound_adapter<key_compare>(comp);
    return use_linear_search::value ? linear_search(k, upper_compare).value
                                    : binary_search(k, upper_compare).value;
  }

  template <typename K, typename Compare>
  SearchResult<int, btree_is_key_compare_to<Compare, key_type>::value>
  linear_search(const K &k, const Compare &comp) const {
    return linear_search_impl(k, start(), finish(), comp,
                              btree_is_key_compare_to<Compare, key_type>());
  }

  template <typename K, typename Compare>
  SearchResult<int, btree_is_key_compare_to<Compare, key_type>::value>
  binary_search(const K &k, const Compare &comp) const {
    return binary_search_impl(k, start(), finish(), comp,
                              btree_is_key_compare_to<Compare, key_type>());
  }

  // Returns the position of the first value whose key is not less than k using
  // linear search performed using plain compare.
  template <typename K, typename Compare>
  SearchResult<int, false> linear_search_impl(
      const K &k, int s, const int e, const Compare &comp,
      std::false_type /* IsCompareTo */) const {
    while (s < e) {
      if (!comp(key(s), k)) {
        break;
      }
      ++s;
    }
    return SearchResult<int, false>{s};
  }

  // Returns the position of the first value whose key is not less than k using
  // linear search performed using compare-to.
  template <typename K, typename Compare>
  SearchResult<int, true> linear_search_impl(
      const K &k, int s, const int e, const Compare &comp,
      std::true_type /* IsCompareTo */) const {
    while (s < e) {
      const absl::weak_ordering c = comp(key(s), k);
      if (c == 0) {
        return {s, MatchKind::kEq};
      } else if (c > 0) {
        break;
      }
      ++s;
    }
    return {s, MatchKind::kNe};
  }

  // Returns the position of the first value whose key is not less than k using
  // binary search performed using plain compare.
  template <typename K, typename Compare>
  SearchResult<int, false> binary_search_impl(
      const K &k, int s, int e, const Compare &comp,
      std::false_type /* IsCompareTo */) const {
    while (s != e) {
      const int mid = (s + e) >> 1;
      if (comp(key(mid), k)) {
        s = mid + 1;
      } else {
        e = mid;
      }
    }
    return SearchResult<int, false>{s};
  }

  // Returns the position of the first value whose key is not less than k using
  // binary search performed using compare-to.
  template <typename K, typename CompareTo>
  SearchResult<int, true> binary_search_impl(
      const K &k, int s, int e, const CompareTo &comp,
      std::true_type /* IsCompareTo */) const {
    if (params_type::template can_have_multiple_equivalent_keys<K>()) {
      MatchKind exact_match = MatchKind::kNe;
      while (s != e) {
        const int mid = (s + e) >> 1;
        const absl::weak_ordering c = comp(key(mid), k);
        if (c < 0) {
          s = mid + 1;
        } else {
          e = mid;
          if (c == 0) {
            // Need to return the first value whose key is not less than k,
            // which requires continuing the binary search if there could be
            // multiple equivalent keys.
            exact_match = MatchKind::kEq;
          }
        }
      }
      return {s, exact_match};
    } else {  // Can't have multiple equivalent keys.
      while (s != e) {
        const int mid = (s + e) >> 1;
        const absl::weak_ordering c = comp(key(mid), k);
        if (c < 0) {
          s = mid + 1;
        } else if (c > 0) {
          e = mid;
        } else {
          return {mid, MatchKind::kEq};
        }
      }
      return {s, MatchKind::kNe};
    }
  }

  // Emplaces a value at position i, shifting all existing values and
  // children at positions >= i to the right by 1.
  template <typename... Args>
  void emplace_value(size_type i, allocator_type *alloc, Args &&... args);

  // Removes the values at positions [i, i + to_erase), shifting all existing
  // values and children after that range to the left by to_erase. Clears all
  // children between [i, i + to_erase).
  void remove_values(field_type i, field_type to_erase, allocator_type *alloc);

  // Rebalances a node with its right sibling.
  void rebalance_right_to_left(int to_move, btree_node *right,
                               allocator_type *alloc);
  void rebalance_left_to_right(int to_move, btree_node *right,
                               allocator_type *alloc);

  // Splits a node, moving a portion of the node's values to its right sibling.
  void split(int insert_position, btree_node *dest, allocator_type *alloc);

  // Merges a node with its right sibling, moving all of the values and the
  // delimiting key in the parent node onto itself, and deleting the src node.
  void merge(btree_node *src, allocator_type *alloc);

  // Node allocation/deletion routines.
  void init_leaf(btree_node *parent, int max_count) {
    set_parent(parent);
    set_position(0);
    set_start(0);
    set_finish(0);
    set_max_count(max_count);
    absl::container_internal::SanitizerPoisonMemoryRegion(
        start_slot(), max_count * sizeof(slot_type));
  }
  void init_internal(btree_node *parent) {
    init_leaf(parent, kNodeSlots);
    // Set `max_count` to a sentinel value to indicate that this node is
    // internal.
    set_max_count(kInternalNodeMaxCount);
    absl::container_internal::SanitizerPoisonMemoryRegion(
        &mutable_child(start()), (kNodeSlots + 1) * sizeof(btree_node *));
  }

  static void deallocate(const size_type size, btree_node *node,
                         allocator_type *alloc) {
    absl::container_internal::Deallocate<Alignment()>(alloc, node, size);
  }

  // Deletes a node and all of its children.
  static void clear_and_delete(btree_node *node, allocator_type *alloc);

 private:
  template <typename... Args>
  void value_init(const field_type i, allocator_type *alloc, Args &&... args) {
    absl::container_internal::SanitizerUnpoisonObject(slot(i));
    params_type::construct(alloc, slot(i), std::forward<Args>(args)...);
  }
  void value_destroy(const field_type i, allocator_type *alloc) {
    params_type::destroy(alloc, slot(i));
    absl::container_internal::SanitizerPoisonObject(slot(i));
  }
  void value_destroy_n(const field_type i, const field_type n,
                       allocator_type *alloc) {
    for (slot_type *s = slot(i), *end = slot(i + n); s != end; ++s) {
      params_type::destroy(alloc, s);
      absl::container_internal::SanitizerPoisonObject(s);
    }
  }

  static void transfer(slot_type *dest, slot_type *src, allocator_type *alloc) {
    absl::container_internal::SanitizerUnpoisonObject(dest);
    params_type::transfer(alloc, dest, src);
    absl::container_internal::SanitizerPoisonObject(src);
  }

  // Transfers value from slot `src_i` in `src_node` to slot `dest_i` in `this`.
  void transfer(const size_type dest_i, const size_type src_i,
                btree_node *src_node, allocator_type *alloc) {
    transfer(slot(dest_i), src_node->slot(src_i), alloc);
  }

  // Transfers `n` values starting at value `src_i` in `src_node` into the
  // values starting at value `dest_i` in `this`.
  void transfer_n(const size_type n, const size_type dest_i,
                  const size_type src_i, btree_node *src_node,
                  allocator_type *alloc) {
    for (slot_type *src = src_node->slot(src_i), *end = src + n,
                   *dest = slot(dest_i);
         src != end; ++src, ++dest) {
      transfer(dest, src, alloc);
    }
  }

  // Same as above, except that we start at the end and work our way to the
  // beginning.
  void transfer_n_backward(const size_type n, const size_type dest_i,
                           const size_type src_i, btree_node *src_node,
                           allocator_type *alloc) {
    for (slot_type *src = src_node->slot(src_i + n - 1), *end = src - n,
                   *dest = slot(dest_i + n - 1);
         src != end; --src, --dest) {
      transfer(dest, src, alloc);
    }
  }

  template <typename P>
  friend class btree;
  template <typename N, typename R, typename P>
  friend struct btree_iterator;
  friend class BtreeNodePeer;
};

template <typename Node, typename Reference, typename Pointer>
struct btree_iterator {
 private:
  using key_type = typename Node::key_type;
  using size_type = typename Node::size_type;
  using params_type = typename Node::params_type;
  using is_map_container = typename params_type::is_map_container;

  using node_type = Node;
  using normal_node = typename std::remove_const<Node>::type;
  using const_node = const Node;
  using normal_pointer = typename params_type::pointer;
  using normal_reference = typename params_type::reference;
  using const_pointer = typename params_type::const_pointer;
  using const_reference = typename params_type::const_reference;
  using slot_type = typename params_type::slot_type;

  using iterator =
     btree_iterator<normal_node, normal_reference, normal_pointer>;
  using const_iterator =
      btree_iterator<const_node, const_reference, const_pointer>;

 public:
  // These aliases are public for std::iterator_traits.
  using difference_type = typename Node::difference_type;
  using value_type = typename params_type::value_type;
  using pointer = Pointer;
  using reference = Reference;
  using iterator_category = std::bidirectional_iterator_tag;

  btree_iterator() : node(nullptr), position(-1) {}
  explicit btree_iterator(Node *n) : node(n), position(n->start()) {}
  btree_iterator(Node *n, int p) : node(n), position(p) {}

  // NOTE: this SFINAE allows for implicit conversions from iterator to
  // const_iterator, but it specifically avoids hiding the copy constructor so
  // that the trivial one will be used when possible.
  template <typename N, typename R, typename P,
            absl::enable_if_t<
                std::is_same<btree_iterator<N, R, P>, iterator>::value &&
                    std::is_same<btree_iterator, const_iterator>::value,
                int> = 0>
  btree_iterator(const btree_iterator<N, R, P> other)  // NOLINT
      : node(other.node), position(other.position) {}

 private:
  // This SFINAE allows explicit conversions from const_iterator to
  // iterator, but also avoids hiding the copy constructor.
  // NOTE: the const_cast is safe because this constructor is only called by
  // non-const methods and the container owns the nodes.
  template <typename N, typename R, typename P,
            absl::enable_if_t<
                std::is_same<btree_iterator<N, R, P>, const_iterator>::value &&
                    std::is_same<btree_iterator, iterator>::value,
                int> = 0>
  explicit btree_iterator(const btree_iterator<N, R, P> other)
      : node(const_cast<node_type *>(other.node)), position(other.position) {}

  // Increment/decrement the iterator.
  void increment() {
    if (node->leaf() && ++position < node->finish()) {
      return;
    }
    increment_slow();
  }
  void increment_slow();

  void decrement() {
    if (node->leaf() && --position >= node->start()) {
      return;
    }
    decrement_slow();
  }
  void decrement_slow();

 public:
  bool operator==(const iterator &other) const {
    return node == other.node && position == other.position;
  }
  bool operator==(const const_iterator &other) const {
    return node == other.node && position == other.position;
  }
  bool operator!=(const iterator &other) const {
    return node != other.node || position != other.position;
  }
  bool operator!=(const const_iterator &other) const {
    return node != other.node || position != other.position;
  }

  // Accessors for the key/value the iterator is pointing at.
  reference operator*() const {
    ABSL_HARDENING_ASSERT(node != nullptr);
    ABSL_HARDENING_ASSERT(node->start() <= position);
    ABSL_HARDENING_ASSERT(node->finish() > position);
    return node->value(position);
  }
  pointer operator->() const { return &operator*(); }

  btree_iterator &operator++() {
    increment();
    return *this;
  }
  btree_iterator &operator--() {
    decrement();
    return *this;
  }
  btree_iterator operator++(int) {
    btree_iterator tmp = *this;
    ++*this;
    return tmp;
  }
  btree_iterator operator--(int) {
    btree_iterator tmp = *this;
    --*this;
    return tmp;
  }

 private:
  friend iterator;
  friend const_iterator;
  template <typename Params>
  friend class btree;
  template <typename Tree>
  friend class btree_container;
  template <typename Tree>
  friend class btree_set_container;
  template <typename Tree>
  friend class btree_map_container;
  template <typename Tree>
  friend class btree_multiset_container;
  template <typename TreeType, typename CheckerType>
  friend class base_checker;

  const key_type &key() const { return node->key(position); }
  slot_type *slot() { return node->slot(position); }

  // The node in the tree the iterator is pointing at.
  Node *node;
  // The position within the node of the tree the iterator is pointing at.
  // NOTE: this is an int rather than a field_type because iterators can point
  // to invalid positions (such as -1) in certain circumstances.
  int position;
};

template <typename Params>
class btree {
  using node_type = btree_node<Params>;
  using is_key_compare_to = typename Params::is_key_compare_to;
  using init_type = typename Params::init_type;
  using field_type = typename node_type::field_type;

  // We use a static empty node for the root/leftmost/rightmost of empty btrees
  // in order to avoid branching in begin()/end().
  struct alignas(node_type::Alignment()) EmptyNodeType : node_type {
    using field_type = typename node_type::field_type;
    node_type *parent;
    field_type position = 0;
    field_type start = 0;
    field_type finish = 0;
    // max_count must be != kInternalNodeMaxCount (so that this node is regarded
    // as a leaf node). max_count() is never called when the tree is empty.
    field_type max_count = node_type::kInternalNodeMaxCount + 1;

#ifdef _MSC_VER
    // MSVC has constexpr code generations bugs here.
    EmptyNodeType() : parent(this) {}
#else
    constexpr EmptyNodeType(node_type *p) : parent(p) {}
#endif
  };

  static node_type *EmptyNode() {
#ifdef _MSC_VER
    static EmptyNodeType *empty_node = new EmptyNodeType;
    // This assert fails on some other construction methods.
    assert(empty_node->parent == empty_node);
    return empty_node;
#else
    static constexpr EmptyNodeType empty_node(
        const_cast<EmptyNodeType *>(&empty_node));
    return const_cast<EmptyNodeType *>(&empty_node);
#endif
  }

  enum : uint32_t {
    kNodeSlots = node_type::kNodeSlots,
    kMinNodeValues = kNodeSlots / 2,
  };

  struct node_stats {
    using size_type = typename Params::size_type;

    node_stats(size_type l, size_type i) : leaf_nodes(l), internal_nodes(i) {}

    node_stats &operator+=(const node_stats &other) {
      leaf_nodes += other.leaf_nodes;
      internal_nodes += other.internal_nodes;
      return *this;
    }

    size_type leaf_nodes;
    size_type internal_nodes;
  };

 public:
  using key_type = typename Params::key_type;
  using value_type = typename Params::value_type;
  using size_type = typename Params::size_type;
  using difference_type = typename Params::difference_type;
  using key_compare = typename Params::key_compare;
  using original_key_compare = typename Params::original_key_compare;
  using value_compare = typename Params::value_compare;
  using allocator_type = typename Params::allocator_type;
  using reference = typename Params::reference;
  using const_reference = typename Params::const_reference;
  using pointer = typename Params::pointer;
  using const_pointer = typename Params::const_pointer;
  using iterator =
      typename btree_iterator<node_type, reference, pointer>::iterator;
  using const_iterator = typename iterator::const_iterator;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;
  using node_handle_type = node_handle<Params, Params, allocator_type>;

  // Internal types made public for use by btree_container types.
  using params_type = Params;
  using slot_type = typename Params::slot_type;

 private:
  // For use in copy_or_move_values_in_order.
  const value_type &maybe_move_from_iterator(const_iterator it) { return *it; }
  value_type &&maybe_move_from_iterator(iterator it) {
    // This is a destructive operation on the other container so it's safe for
    // us to const_cast and move from the keys here even if it's a set.
    return std::move(const_cast<value_type &>(*it));
  }

  // Copies or moves (depending on the template parameter) the values in
  // other into this btree in their order in other. This btree must be empty
  // before this method is called. This method is used in copy construction,
  // copy assignment, and move assignment.
  template <typename Btree>
  void copy_or_move_values_in_order(Btree &other);

  // Validates that various assumptions/requirements are true at compile time.
  constexpr static bool static_assert_validation();

 public:
  btree(const key_compare &comp, const allocator_type &alloc)
      : root_(comp, alloc, EmptyNode()), rightmost_(EmptyNode()), size_(0) {}

  btree(const btree &other) : btree(other, other.allocator()) {}
  btree(const btree &other, const allocator_type &alloc)
      : btree(other.key_comp(), alloc) {
    copy_or_move_values_in_order(other);
  }
  btree(btree &&other) noexcept
      : root_(std::move(other.root_)),
        rightmost_(absl::exchange(other.rightmost_, EmptyNode())),
        size_(absl::exchange(other.size_, 0)) {
    other.mutable_root() = EmptyNode();
  }
  btree(btree &&other, const allocator_type &alloc)
      : btree(other.key_comp(), alloc) {
    if (alloc == other.allocator()) {
      swap(other);
    } else {
      // Move values from `other` one at a time when allocators are different.
      copy_or_move_values_in_order(other);
    }
  }

  ~btree() {
    // Put static_asserts in destructor to avoid triggering them before the type
    // is complete.
    static_assert(static_assert_validation(), "This call must be elided.");
    clear();
  }

  // Assign the contents of other to *this.
  btree &operator=(const btree &other);
  btree &operator=(btree &&other) noexcept;

  iterator begin() { return iterator(leftmost()); }
  const_iterator begin() const { return const_iterator(leftmost()); }
  iterator end() { return iterator(rightmost_, rightmost_->finish()); }
  const_iterator end() const {
    return const_iterator(rightmost_, rightmost_->finish());
  }
  reverse_iterator rbegin() { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const {
    return const_reverse_iterator(end());
  }
  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const {
    return const_reverse_iterator(begin());
  }

  // Finds the first element whose key is not less than `key`.
  template <typename K>
  iterator lower_bound(const K &key) {
    return internal_end(internal_lower_bound(key).value);
  }
  template <typename K>
  const_iterator lower_bound(const K &key) const {
    return internal_end(internal_lower_bound(key).value);
  }

  // Finds the first element whose key is not less than `key` and also returns
  // whether that element is equal to `key`.
  template <typename K>
  std::pair<iterator, bool> lower_bound_equal(const K &key) const;

  // Finds the first element whose key is greater than `key`.
  template <typename K>
  iterator upper_bound(const K &key) {
    return internal_end(internal_upper_bound(key));
  }
  template <typename K>
  const_iterator upper_bound(const K &key) const {
    return internal_end(internal_upper_bound(key));
  }

  // Finds the range of values which compare equal to key. The first member of
  // the returned pair is equal to lower_bound(key). The second member of the
  // pair is equal to upper_bound(key).
  template <typename K>
  std::pair<iterator, iterator> equal_range(const K &key);
  template <typename K>
  std::pair<const_iterator, const_iterator> equal_range(const K &key) const {
    return const_cast<btree *>(this)->equal_range(key);
  }

  // Inserts a value into the btree only if it does not already exist. The
  // boolean return value indicates whether insertion succeeded or failed.
  // Requirement: if `key` already exists in the btree, does not consume `args`.
  // Requirement: `key` is never referenced after consuming `args`.
  template <typename K, typename... Args>
  std::pair<iterator, bool> insert_unique(const K &key, Args &&... args);

  // Inserts with hint. Checks to see if the value should be placed immediately
  // before `position` in the tree. If so, then the insertion will take
  // amortized constant time. If not, the insertion will take amortized
  // logarithmic time as if a call to insert_unique() were made.
  // Requirement: if `key` already exists in the btree, does not consume `args`.
  // Requirement: `key` is never referenced after consuming `args`.
  template <typename K, typename... Args>
  std::pair<iterator, bool> insert_hint_unique(iterator position,
                                               const K &key,
                                               Args &&... args);

  // Insert a range of values into the btree.
  // Note: the first overload avoids constructing a value_type if the key
  // already exists in the btree.
  template <typename InputIterator,
            typename = decltype(std::declval<const key_compare &>()(
                params_type::key(*std::declval<InputIterator>()),
                std::declval<const key_type &>()))>
  void insert_iterator_unique(InputIterator b, InputIterator e, int);
  // We need the second overload for cases in which we need to construct a
  // value_type in order to compare it with the keys already in the btree.
  template <typename InputIterator>
  void insert_iterator_unique(InputIterator b, InputIterator e, char);

  // Inserts a value into the btree.
  template <typename ValueType>
  iterator insert_multi(const key_type &key, ValueType &&v);

  // Inserts a value into the btree.
  template <typename ValueType>
  iterator insert_multi(ValueType &&v) {
    return insert_multi(params_type::key(v), std::forward<ValueType>(v));
  }

  // Insert with hint. Check to see if the value should be placed immediately
  // before position in the tree. If it does, then the insertion will take
  // amortized constant time. If not, the insertion will take amortized
  // logarithmic time as if a call to insert_multi(v) were made.
  template <typename ValueType>
  iterator insert_hint_multi(iterator position, ValueType &&v);

  // Insert a range of values into the btree.
  template <typename InputIterator>
  void insert_iterator_multi(InputIterator b, InputIterator e);

  // Erase the specified iterator from the btree. The iterator must be valid
  // (i.e. not equal to end()).  Return an iterator pointing to the node after
  // the one that was erased (or end() if none exists).
  // Requirement: does not read the value at `*iter`.
  iterator erase(iterator iter);

  // Erases range. Returns the number of keys erased and an iterator pointing
  // to the element after the last erased element.
  std::pair<size_type, iterator> erase_range(iterator begin, iterator end);

  // Finds an element with key equivalent to `key` or returns `end()` if `key`
  // is not present.
  template <typename K>
  iterator find(const K &key) {
    return internal_end(internal_find(key));
  }
  template <typename K>
  const_iterator find(const K &key) const {
    return internal_end(internal_find(key));
  }

  // Clear the btree, deleting all of the values it contains.
  void clear();

  // Swaps the contents of `this` and `other`.
  void swap(btree &other);

  const key_compare &key_comp() const noexcept {
    return root_.template get<0>();
  }
  template <typename K1, typename K2>
  bool compare_keys(const K1 &a, const K2 &b) const {
    return compare_internal::compare_result_as_less_than(key_comp()(a, b));
  }

  value_compare value_comp() const {
    return value_compare(original_key_compare(key_comp()));
  }

  // Verifies the structure of the btree.
  void verify() const;

  // Size routines.
  size_type size() const { return size_; }
  size_type max_size() const { return (std::numeric_limits<size_type>::max)(); }
  bool empty() const { return size_ == 0; }

  // The height of the btree. An empty tree will have height 0.
  size_type height() const {
    size_type h = 0;
    if (!empty()) {
      // Count the length of the chain from the leftmost node up to the
      // root. We actually count from the root back around to the level below
      // the root, but the calculation is the same because of the circularity
      // of that traversal.
      const node_type *n = root();
      do {
        ++h;
        n = n->parent();
      } while (n != root());
    }
    return h;
  }

  // The number of internal, leaf and total nodes used by the btree.
  size_type leaf_nodes() const { return internal_stats(root()).leaf_nodes; }
  size_type internal_nodes() const {
    return internal_stats(root()).internal_nodes;
  }
  size_type nodes() const {
    node_stats stats = internal_stats(root());
    return stats.leaf_nodes + stats.internal_nodes;
  }

  // The total number of bytes used by the btree.
  size_type bytes_used() const {
    node_stats stats = internal_stats(root());
    if (stats.leaf_nodes == 1 && stats.internal_nodes == 0) {
      return sizeof(*this) + node_type::LeafSize(root()->max_count());
    } else {
      return sizeof(*this) + stats.leaf_nodes * node_type::LeafSize() +
             stats.internal_nodes * node_type::InternalSize();
    }
  }

  // The average number of bytes used per value stored in the btree assuming
  // random insertion order.
  static double average_bytes_per_value() {
    // The expected number of values per node with random insertion order is the
    // average of the maximum and minimum numbers of values per node.
    const double expected_values_per_node =
        (kNodeSlots + kMinNodeValues) / 2.0;
    return node_type::LeafSize() / expected_values_per_node;
  }

  // The fullness of the btree. Computed as the number of elements in the btree
  // divided by the maximum number of elements a tree with the current number
  // of nodes could hold. A value of 1 indicates perfect space
  // utilization. Smaller values indicate space wastage.
  // Returns 0 for empty trees.
  double fullness() const {
    if (empty()) return 0.0;
    return static_cast<double>(size()) / (nodes() * kNodeSlots);
  }
  // The overhead of the btree structure in bytes per node. Computed as the
  // total number of bytes used by the btree minus the number of bytes used for
  // storing elements divided by the number of elements.
  // Returns 0 for empty trees.
  double overhead() const {
    if (empty()) return 0.0;
    return (bytes_used() - size() * sizeof(value_type)) /
           static_cast<double>(size());
  }

  // The allocator used by the btree.
  allocator_type get_allocator() const { return allocator(); }

 private:
  // Internal accessor routines.
  node_type *root() { return root_.template get<2>(); }
  const node_type *root() const { return root_.template get<2>(); }
  node_type *&mutable_root() noexcept { return root_.template get<2>(); }
  key_compare *mutable_key_comp() noexcept { return &root_.template get<0>(); }

  // The leftmost node is stored as the parent of the root node.
  node_type *leftmost() { return root()->parent(); }
  const node_type *leftmost() const { return root()->parent(); }

  // Allocator routines.
  allocator_type *mutable_allocator() noexcept {
    return &root_.template get<1>();
  }
  const allocator_type &allocator() const noexcept {
    return root_.template get<1>();
  }

  // Allocates a correctly aligned node of at least size bytes using the
  // allocator.
  node_type *allocate(const size_type size) {
    return reinterpret_cast<node_type *>(
        absl::container_internal::Allocate<node_type::Alignment()>(
            mutable_allocator(), size));
  }

  // Node creation/deletion routines.
  node_type *new_internal_node(node_type *parent) {
    node_type *n = allocate(node_type::InternalSize());
    n->init_internal(parent);
    return n;
  }
  node_type *new_leaf_node(node_type *parent) {
    node_type *n = allocate(node_type::LeafSize());
    n->init_leaf(parent, kNodeSlots);
    return n;
  }
  node_type *new_leaf_root_node(const int max_count) {
    node_type *n = allocate(node_type::LeafSize(max_count));
    n->init_leaf(/*parent=*/n, max_count);
    return n;
  }

  // Deletion helper routines.
  iterator rebalance_after_delete(iterator iter);

  // Rebalances or splits the node iter points to.
  void rebalance_or_split(iterator *iter);

  // Merges the values of left, right and the delimiting key on their parent
  // onto left, removing the delimiting key and deleting right.
  void merge_nodes(node_type *left, node_type *right);

  // Tries to merge node with its left or right sibling, and failing that,
  // rebalance with its left or right sibling. Returns true if a merge
  // occurred, at which point it is no longer valid to access node. Returns
  // false if no merging took place.
  bool try_merge_or_rebalance(iterator *iter);

  // Tries to shrink the height of the tree by 1.
  void try_shrink();

  iterator internal_end(iterator iter) {
    return iter.node != nullptr ? iter : end();
  }
  const_iterator internal_end(const_iterator iter) const {
    return iter.node != nullptr ? iter : end();
  }

  // Emplaces a value into the btree immediately before iter. Requires that
  // key(v) <= iter.key() and (--iter).key() <= key(v).
  template <typename... Args>
  iterator internal_emplace(iterator iter, Args &&... args);

  // Returns an iterator pointing to the first value >= the value "iter" is
  // pointing at. Note that "iter" might be pointing to an invalid location such
  // as iter.position == iter.node->finish(). This routine simply moves iter up
  // in the tree to a valid location.
  // Requires: iter.node is non-null.
  template <typename IterType>
  static IterType internal_last(IterType iter);

  // Returns an iterator pointing to the leaf position at which key would
  // reside in the tree, unless there is an exact match - in which case, the
  // result may not be on a leaf. When there's a three-way comparator, we can
  // return whether there was an exact match. This allows the caller to avoid a
  // subsequent comparison to determine if an exact match was made, which is
  // important for keys with expensive comparison, such as strings.
  template <typename K>
  SearchResult<iterator, is_key_compare_to::value> internal_locate(
      const K &key) const;

  // Internal routine which implements lower_bound().
  template <typename K>
  SearchResult<iterator, is_key_compare_to::value> internal_lower_bound(
      const K &key) const;

  // Internal routine which implements upper_bound().
  template <typename K>
  iterator internal_upper_bound(const K &key) const;

  // Internal routine which implements find().
  template <typename K>
  iterator internal_find(const K &key) const;

  // Verifies the tree structure of node.
  int internal_verify(const node_type *node, const key_type *lo,
                      const key_type *hi) const;

  node_stats internal_stats(const node_type *node) const {
    // The root can be a static empty node.
    if (node == nullptr || (node == root() && empty())) {
      return node_stats(0, 0);
    }
    if (node->leaf()) {
      return node_stats(1, 0);
    }
    node_stats res(0, 1);
    for (int i = node->start(); i <= node->finish(); ++i) {
      res += internal_stats(node->child(i));
    }
    return res;
  }

  // We use compressed tuple in order to save space because key_compare and
  // allocator_type are usually empty.
  absl::container_internal::CompressedTuple<key_compare, allocator_type,
                                            node_type *>
      root_;

  // A pointer to the rightmost node. Note that the leftmost node is stored as
  // the root's parent.
  node_type *rightmost_;

  // Number of values.
  size_type size_;
};

////
// btree_node methods
template <typename P>
template <typename... Args>
inline void btree_node<P>::emplace_value(const size_type i,
                                         allocator_type *alloc,
                                         Args &&... args) {
  assert(i >= start());
  assert(i <= finish());
  // Shift old values to create space for new value and then construct it in
  // place.
  if (i < finish()) {
    transfer_n_backward(finish() - i, /*dest_i=*/i + 1, /*src_i=*/i, this,
                        alloc);
  }
  value_init(i, alloc, std::forward<Args>(args)...);
  set_finish(finish() + 1);

  if (!leaf() && finish() > i + 1) {
    for (int j = finish(); j > i + 1; --j) {
      set_child(j, child(j - 1));
    }
    clear_child(i + 1);
  }
}

template <typename P>
inline void btree_node<P>::remove_values(const field_type i,
                                         const field_type to_erase,
                                         allocator_type *alloc) {
  // Transfer values after the removed range into their new places.
  value_destroy_n(i, to_erase, alloc);
  const field_type orig_finish = finish();
  const field_type src_i = i + to_erase;
  transfer_n(orig_finish - src_i, i, src_i, this, alloc);

  if (!leaf()) {
    // Delete all children between begin and end.
    for (int j = 0; j < to_erase; ++j) {
      clear_and_delete(child(i + j + 1), alloc);
    }
    // Rotate children after end into new positions.
    for (int j = i + to_erase + 1; j <= orig_finish; ++j) {
      set_child(j - to_erase, child(j));
      clear_child(j);
    }
  }
  set_finish(orig_finish - to_erase);
}

template <typename P>
void btree_node<P>::rebalance_right_to_left(const int to_move,
                                            btree_node *right,
                                            allocator_type *alloc) {
  assert(parent() == right->parent());
  assert(position() + 1 == right->position());
  assert(right->count() >= count());
  assert(to_move >= 1);
  assert(to_move <= right->count());

  // 1) Move the delimiting value in the parent to the left node.
  transfer(finish(), position(), parent(), alloc);

  // 2) Move the (to_move - 1) values from the right node to the left node.
  transfer_n(to_move - 1, finish() + 1, right->start(), right, alloc);

  // 3) Move the new delimiting value to the parent from the right node.
  parent()->transfer(position(), right->start() + to_move - 1, right, alloc);

  // 4) Shift the values in the right node to their correct positions.
  right->transfer_n(right->count() - to_move, right->start(),
                    right->start() + to_move, right, alloc);

  if (!leaf()) {
    // Move the child pointers from the right to the left node.
    for (int i = 0; i < to_move; ++i) {
      init_child(finish() + i + 1, right->child(i));
    }
    for (int i = right->start(); i <= right->finish() - to_move; ++i) {
      assert(i + to_move <= right->max_count());
      right->init_child(i, right->child(i + to_move));
      right->clear_child(i + to_move);
    }
  }

  // Fixup `finish` on the left and right nodes.
  set_finish(finish() + to_move);
  right->set_finish(right->finish() - to_move);
}

template <typename P>
void btree_node<P>::rebalance_left_to_right(const int to_move,
                                            btree_node *right,
                                            allocator_type *alloc) {
  assert(parent() == right->parent());
  assert(position() + 1 == right->position());
  assert(count() >= right->count());
  assert(to_move >= 1);
  assert(to_move <= count());

  // Values in the right node are shifted to the right to make room for the
  // new to_move values. Then, the delimiting value in the parent and the
  // other (to_move - 1) values in the left node are moved into the right node.
  // Lastly, a new delimiting value is moved from the left node into the
  // parent, and the remaining empty left node entries are destroyed.

  // 1) Shift existing values in the right node to their correct positions.
  right->transfer_n_backward(right->count(), right->start() + to_move,
                             right->start(), right, alloc);

  // 2) Move the delimiting value in the parent to the right node.
  right->transfer(right->start() + to_move - 1, position(), parent(), alloc);

  // 3) Move the (to_move - 1) values from the left node to the right node.
  right->transfer_n(to_move - 1, right->start(), finish() - (to_move - 1), this,
                    alloc);

  // 4) Move the new delimiting value to the parent from the left node.
  parent()->transfer(position(), finish() - to_move, this, alloc);

  if (!leaf()) {
    // Move the child pointers from the left to the right node.
    for (int i = right->finish(); i >= right->start(); --i) {
      right->init_child(i + to_move, right->child(i));
      right->clear_child(i);
    }
    for (int i = 1; i <= to_move; ++i) {
      right->init_child(i - 1, child(finish() - to_move + i));
      clear_child(finish() - to_move + i);
    }
  }

  // Fixup the counts on the left and right nodes.
  set_finish(finish() - to_move);
  right->set_finish(right->finish() + to_move);
}

template <typename P>
void btree_node<P>::split(const int insert_position, btree_node *dest,
                          allocator_type *alloc) {
  assert(dest->count() == 0);
  assert(max_count() == kNodeSlots);

  // We bias the split based on the position being inserted. If we're
  // inserting at the beginning of the left node then bias the split to put
  // more values on the right node. If we're inserting at the end of the
  // right node then bias the split to put more values on the left node.
  if (insert_position == start()) {
    dest->set_finish(dest->start() + finish() - 1);
  } else if (insert_position == kNodeSlots) {
    dest->set_finish(dest->start());
  } else {
    dest->set_finish(dest->start() + count() / 2);
  }
  set_finish(finish() - dest->count());
  assert(count() >= 1);

  // Move values from the left sibling to the right sibling.
  dest->transfer_n(dest->count(), dest->start(), finish(), this, alloc);

  // The split key is the largest value in the left sibling.
  --mutable_finish();
  parent()->emplace_value(position(), alloc, finish_slot());
  value_destroy(finish(), alloc);
  parent()->init_child(position() + 1, dest);

  if (!leaf()) {
    for (int i = dest->start(), j = finish() + 1; i <= dest->finish();
         ++i, ++j) {
      assert(child(j) != nullptr);
      dest->init_child(i, child(j));
      clear_child(j);
    }
  }
}

template <typename P>
void btree_node<P>::merge(btree_node *src, allocator_type *alloc) {
  assert(parent() == src->parent());
  assert(position() + 1 == src->position());

  // Move the delimiting value to the left node.
  value_init(finish(), alloc, parent()->slot(position()));

  // Move the values from the right to the left node.
  transfer_n(src->count(), finish() + 1, src->start(), src, alloc);

  if (!leaf()) {
    // Move the child pointers from the right to the left node.
    for (int i = src->start(), j = finish() + 1; i <= src->finish(); ++i, ++j) {
      init_child(j, src->child(i));
      src->clear_child(i);
    }
  }

  // Fixup `finish` on the src and dest nodes.
  set_finish(start() + 1 + count() + src->count());
  src->set_finish(src->start());

  // Remove the value on the parent node and delete the src node.
  parent()->remove_values(position(), /*to_erase=*/1, alloc);
}

template <typename P>
void btree_node<P>::clear_and_delete(btree_node *node, allocator_type *alloc) {
  if (node->leaf()) {
    node->value_destroy_n(node->start(), node->count(), alloc);
    deallocate(LeafSize(node->max_count()), node, alloc);
    return;
  }
  if (node->count() == 0) {
    deallocate(InternalSize(), node, alloc);
    return;
  }

  // The parent of the root of the subtree we are deleting.
  btree_node *delete_root_parent = node->parent();

  // Navigate to the leftmost leaf under node, and then delete upwards.
  while (!node->leaf()) node = node->start_child();
  // Use `int` because `pos` needs to be able to hold `kNodeSlots+1`, which
  // isn't guaranteed to be a valid `field_type`.
  int pos = node->position();
  btree_node *parent = node->parent();
  for (;;) {
    // In each iteration of the next loop, we delete one leaf node and go right.
    assert(pos <= parent->finish());
    do {
      node = parent->child(pos);
      if (!node->leaf()) {
        // Navigate to the leftmost leaf under node.
        while (!node->leaf()) node = node->start_child();
        pos = node->position();
        parent = node->parent();
      }
      node->value_destroy_n(node->start(), node->count(), alloc);
      deallocate(LeafSize(node->max_count()), node, alloc);
      ++pos;
    } while (pos <= parent->finish());

    // Once we've deleted all children of parent, delete parent and go up/right.
    assert(pos > parent->finish());
    do {
      node = parent;
      pos = node->position();
      parent = node->parent();
      node->value_destroy_n(node->start(), node->count(), alloc);
      deallocate(InternalSize(), node, alloc);
      if (parent == delete_root_parent) return;
      ++pos;
    } while (pos > parent->finish());
  }
}

////
// btree_iterator methods
template <typename N, typename R, typename P>
void btree_iterator<N, R, P>::increment_slow() {
  if (node->leaf()) {
    assert(position >= node->finish());
    btree_iterator save(*this);
    while (position == node->finish() && !node->is_root()) {
      assert(node->parent()->child(node->position()) == node);
      position = node->position();
      node = node->parent();
    }
    // TODO(ezb): assert we aren't incrementing end() instead of handling.
    if (position == node->finish()) {
      *this = save;
    }
  } else {
    assert(position < node->finish());
    node = node->child(position + 1);
    while (!node->leaf()) {
      node = node->start_child();
    }
    position = node->start();
  }
}

template <typename N, typename R, typename P>
void btree_iterator<N, R, P>::decrement_slow() {
  if (node->leaf()) {
    assert(position <= -1);
    btree_iterator save(*this);
    while (position < node->start() && !node->is_root()) {
      assert(node->parent()->child(node->position()) == node);
      position = node->position() - 1;
      node = node->parent();
    }
    // TODO(ezb): assert we aren't decrementing begin() instead of handling.
    if (position < node->start()) {
      *this = save;
    }
  } else {
    assert(position >= node->start());
    node = node->child(position);
    while (!node->leaf()) {
      node = node->child(node->finish());
    }
    position = node->finish() - 1;
  }
}

////
// btree methods
template <typename P>
template <typename Btree>
void btree<P>::copy_or_move_values_in_order(Btree &other) {
  static_assert(std::is_same<btree, Btree>::value ||
                    std::is_same<const btree, Btree>::value,
                "Btree type must be same or const.");
  assert(empty());

  // We can avoid key comparisons because we know the order of the
  // values is the same order we'll store them in.
  auto iter = other.begin();
  if (iter == other.end()) return;
  insert_multi(maybe_move_from_iterator(iter));
  ++iter;
  for (; iter != other.end(); ++iter) {
    // If the btree is not empty, we can just insert the new value at the end
    // of the tree.
    internal_emplace(end(), maybe_move_from_iterator(iter));
  }
}

template <typename P>
constexpr bool btree<P>::static_assert_validation() {
  static_assert(std::is_nothrow_copy_constructible<key_compare>::value,
                "Key comparison must be nothrow copy constructible");
  static_assert(std::is_nothrow_copy_constructible<allocator_type>::value,
                "Allocator must be nothrow copy constructible");
  static_assert(type_traits_internal::is_trivially_copyable<iterator>::value,
                "iterator not trivially copyable.");

  // Note: We assert that kTargetValues, which is computed from
  // Params::kTargetNodeSize, must fit the node_type::field_type.
  static_assert(
      kNodeSlots < (1 << (8 * sizeof(typename node_type::field_type))),
      "target node size too large");

  // Verify that key_compare returns an absl::{weak,strong}_ordering or bool.
  using compare_result_type =
      absl::result_of_t<key_compare(key_type, key_type)>;
  static_assert(
      std::is_same<compare_result_type, bool>::value ||
          std::is_convertible<compare_result_type, absl::weak_ordering>::value,
      "key comparison function must return absl::{weak,strong}_ordering or "
      "bool.");

  // Test the assumption made in setting kNodeValueSpace.
  static_assert(node_type::MinimumOverhead() >= sizeof(void *) + 4,
                "node space assumption incorrect");

  return true;
}

template <typename P>
template <typename K>
auto btree<P>::lower_bound_equal(const K &key) const
    -> std::pair<iterator, bool> {
  const SearchResult<iterator, is_key_compare_to::value> res =
      internal_lower_bound(key);
  const iterator lower = iterator(internal_end(res.value));
  const bool equal = res.HasMatch()
                         ? res.IsEq()
                         : lower != end() && !compare_keys(key, lower.key());
  return {lower, equal};
}

template <typename P>
template <typename K>
auto btree<P>::equal_range(const K &key) -> std::pair<iterator, iterator> {
  const std::pair<iterator, bool> lower_and_equal = lower_bound_equal(key);
  const iterator lower = lower_and_equal.first;
  if (!lower_and_equal.second) {
    return {lower, lower};
  }

  const iterator next = std::next(lower);
  if (!params_type::template can_have_multiple_equivalent_keys<K>()) {
    // The next iterator after lower must point to a key greater than `key`.
    // Note: if this assert fails, then it may indicate that the comparator does
    // not meet the equivalence requirements for Compare
    // (see https://en.cppreference.com/w/cpp/named_req/Compare).
    assert(next == end() || compare_keys(key, next.key()));
    return {lower, next};
  }
  // Try once more to avoid the call to upper_bound() if there's only one
  // equivalent key. This should prevent all calls to upper_bound() in cases of
  // unique-containers with heterogeneous comparators in which all comparison
  // operators have the same equivalence classes.
  if (next == end() || compare_keys(key, next.key())) return {lower, next};

  // In this case, we need to call upper_bound() to avoid worst case O(N)
  // behavior if we were to iterate over equal keys.
  return {lower, upper_bound(key)};
}

template <typename P>
template <typename K, typename... Args>
auto btree<P>::insert_unique(const K &key, Args &&... args)
    -> std::pair<iterator, bool> {
  if (empty()) {
    mutable_root() = rightmost_ = new_leaf_root_node(1);
  }

  SearchResult<iterator, is_key_compare_to::value> res = internal_locate(key);
  iterator iter = res.value;

  if (res.HasMatch()) {
    if (res.IsEq()) {
      // The key already exists in the tree, do nothing.
      return {iter, false};
    }
  } else {
    iterator last = internal_last(iter);
    if (last.node && !compare_keys(key, last.key())) {
      // The key already exists in the tree, do nothing.
      return {last, false};
    }
  }
  return {internal_emplace(iter, std::forward<Args>(args)...), true};
}

template <typename P>
template <typename K, typename... Args>
inline auto btree<P>::insert_hint_unique(iterator position, const K &key,
                                         Args &&... args)
    -> std::pair<iterator, bool> {
  if (!empty()) {
    if (position == end() || compare_keys(key, position.key())) {
      if (position == begin() || compare_keys(std::prev(position).key(), key)) {
        // prev.key() < key < position.key()
        return {internal_emplace(position, std::forward<Args>(args)...), true};
      }
    } else if (compare_keys(position.key(), key)) {
      ++position;
      if (position == end() || compare_keys(key, position.key())) {
        // {original `position`}.key() < key < {current `position`}.key()
        return {internal_emplace(position, std::forward<Args>(args)...), true};
      }
    } else {
      // position.key() == key
      return {position, false};
    }
  }
  return insert_unique(key, std::forward<Args>(args)...);
}

template <typename P>
template <typename InputIterator, typename>
void btree<P>::insert_iterator_unique(InputIterator b, InputIterator e, int) {
  for (; b != e; ++b) {
    insert_hint_unique(end(), params_type::key(*b), *b);
  }
}

template <typename P>
template <typename InputIterator>
void btree<P>::insert_iterator_unique(InputIterator b, InputIterator e, char) {
  for (; b != e; ++b) {
    init_type value(*b);
    insert_hint_unique(end(), params_type::key(value), std::move(value));
  }
}

template <typename P>
template <typename ValueType>
auto btree<P>::insert_multi(const key_type &key, ValueType &&v) -> iterator {
  if (empty()) {
    mutable_root() = rightmost_ = new_leaf_root_node(1);
  }

  iterator iter = internal_upper_bound(key);
  if (iter.node == nullptr) {
    iter = end();
  }
  return internal_emplace(iter, std::forward<ValueType>(v));
}

template <typename P>
template <typename ValueType>
auto btree<P>::insert_hint_multi(iterator position, ValueType &&v) -> iterator {
  if (!empty()) {
    const key_type &key = params_type::key(v);
    if (position == end() || !compare_keys(position.key(), key)) {
      if (position == begin() ||
          !compare_keys(key, std::prev(position).key())) {
        // prev.key() <= key <= position.key()
        return internal_emplace(position, std::forward<ValueType>(v));
      }
    } else {
      ++position;
      if (position == end() || !compare_keys(position.key(), key)) {
        // {original `position`}.key() < key < {current `position`}.key()
        return internal_emplace(position, std::forward<ValueType>(v));
      }
    }
  }
  return insert_multi(std::forward<ValueType>(v));
}

template <typename P>
template <typename InputIterator>
void btree<P>::insert_iterator_multi(InputIterator b, InputIterator e) {
  for (; b != e; ++b) {
    insert_hint_multi(end(), *b);
  }
}

template <typename P>
auto btree<P>::operator=(const btree &other) -> btree & {
  if (this != &other) {
    clear();

    *mutable_key_comp() = other.key_comp();
    if (absl::allocator_traits<
            allocator_type>::propagate_on_container_copy_assignment::value) {
      *mutable_allocator() = other.allocator();
    }

    copy_or_move_values_in_order(other);
  }
  return *this;
}

template <typename P>
auto btree<P>::operator=(btree &&other) noexcept -> btree & {
  if (this != &other) {
    clear();

    using std::swap;
    if (absl::allocator_traits<
            allocator_type>::propagate_on_container_copy_assignment::value) {
      // Note: `root_` also contains the allocator and the key comparator.
      swap(root_, other.root_);
      swap(rightmost_, other.rightmost_);
      swap(size_, other.size_);
    } else {
      if (allocator() == other.allocator()) {
        swap(mutable_root(), other.mutable_root());
        swap(*mutable_key_comp(), *other.mutable_key_comp());
        swap(rightmost_, other.rightmost_);
        swap(size_, other.size_);
      } else {
        // We aren't allowed to propagate the allocator and the allocator is
        // different so we can't take over its memory. We must move each element
        // individually. We need both `other` and `this` to have `other`s key
        // comparator while moving the values so we can't swap the key
        // comparators.
        *mutable_key_comp() = other.key_comp();
        copy_or_move_values_in_order(other);
      }
    }
  }
  return *this;
}

template <typename P>
auto btree<P>::erase(iterator iter) -> iterator {
  bool internal_delete = false;
  if (!iter.node->leaf()) {
    // Deletion of a value on an internal node. First, move the largest value
    // from our left child here, then delete that position (in remove_values()
    // below). We can get to the largest value from our left child by
    // decrementing iter.
    iterator internal_iter(iter);
    --iter;
    assert(iter.node->leaf());
    params_type::move(mutable_allocator(), iter.node->slot(iter.position),
                      internal_iter.node->slot(internal_iter.position));
    internal_delete = true;
  }

  // Delete the key from the leaf.
  iter.node->remove_values(iter.position, /*to_erase=*/1, mutable_allocator());
  --size_;

  // We want to return the next value after the one we just erased. If we
  // erased from an internal node (internal_delete == true), then the next
  // value is ++(++iter). If we erased from a leaf node (internal_delete ==
  // false) then the next value is ++iter. Note that ++iter may point to an
  // internal node and the value in the internal node may move to a leaf node
  // (iter.node) when rebalancing is performed at the leaf level.

  iterator res = rebalance_after_delete(iter);

  // If we erased from an internal node, advance the iterator.
  if (internal_delete) {
    ++res;
  }
  return res;
}

template <typename P>
auto btree<P>::rebalance_after_delete(iterator iter) -> iterator {
  // Merge/rebalance as we walk back up the tree.
  iterator res(iter);
  bool first_iteration = true;
  for (;;) {
    if (iter.node == root()) {
      try_shrink();
      if (empty()) {
        return end();
      }
      break;
    }
    if (iter.node->count() >= kMinNodeValues) {
      break;
    }
    bool merged = try_merge_or_rebalance(&iter);
    // On the first iteration, we should update `res` with `iter` because `res`
    // may have been invalidated.
    if (first_iteration) {
      res = iter;
      first_iteration = false;
    }
    if (!merged) {
      break;
    }
    iter.position = iter.node->position();
    iter.node = iter.node->parent();
  }

  // Adjust our return value. If we're pointing at the end of a node, advance
  // the iterator.
  if (res.position == res.node->finish()) {
    res.position = res.node->finish() - 1;
    ++res;
  }

  return res;
}

template <typename P>
auto btree<P>::erase_range(iterator begin, iterator end)
    -> std::pair<size_type, iterator> {
  difference_type count = std::distance(begin, end);
  assert(count >= 0);

  if (count == 0) {
    return {0, begin};
  }

  if (count == size_) {
    clear();
    return {count, this->end()};
  }

  if (begin.node == end.node) {
    assert(end.position > begin.position);
    begin.node->remove_values(begin.position, end.position - begin.position,
                              mutable_allocator());
    size_ -= count;
    return {count, rebalance_after_delete(begin)};
  }

  const size_type target_size = size_ - count;
  while (size_ > target_size) {
    if (begin.node->leaf()) {
      const size_type remaining_to_erase = size_ - target_size;
      const size_type remaining_in_node = begin.node->finish() - begin.position;
      const size_type to_erase =
          (std::min)(remaining_to_erase, remaining_in_node);
      begin.node->remove_values(begin.position, to_erase, mutable_allocator());
      size_ -= to_erase;
      begin = rebalance_after_delete(begin);
    } else {
      begin = erase(begin);
    }
  }
  return {count, begin};
}

template <typename P>
void btree<P>::clear() {
  if (!empty()) {
    node_type::clear_and_delete(root(), mutable_allocator());
  }
  mutable_root() = EmptyNode();
  rightmost_ = EmptyNode();
  size_ = 0;
}

template <typename P>
void btree<P>::swap(btree &other) {
  using std::swap;
  if (absl::allocator_traits<
          allocator_type>::propagate_on_container_swap::value) {
    // Note: `root_` also contains the allocator and the key comparator.
    swap(root_, other.root_);
  } else {
    // It's undefined behavior if the allocators are unequal here.
    assert(allocator() == other.allocator());
    swap(mutable_root(), other.mutable_root());
    swap(*mutable_key_comp(), *other.mutable_key_comp());
  }
  swap(rightmost_, other.rightmost_);
  swap(size_, other.size_);
}

template <typename P>
void btree<P>::verify() const {
  assert(root() != nullptr);
  assert(leftmost() != nullptr);
  assert(rightmost_ != nullptr);
  assert(empty() || size() == internal_verify(root(), nullptr, nullptr));
  assert(leftmost() == (++const_iterator(root(), -1)).node);
  assert(rightmost_ == (--const_iterator(root(), root()->finish())).node);
  assert(leftmost()->leaf());
  assert(rightmost_->leaf());
}

template <typename P>
void btree<P>::rebalance_or_split(iterator *iter) {
  node_type *&node = iter->node;
  int &insert_position = iter->position;
  assert(node->count() == node->max_count());
  assert(kNodeSlots == node->max_count());

  // First try to make room on the node by rebalancing.
  node_type *parent = node->parent();
  if (node != root()) {
    if (node->position() > parent->start()) {
      // Try rebalancing with our left sibling.
      node_type *left = parent->child(node->position() - 1);
      assert(left->max_count() == kNodeSlots);
      if (left->count() < kNodeSlots) {
        // We bias rebalancing based on the position being inserted. If we're
        // inserting at the end of the right node then we bias rebalancing to
        // fill up the left node.
        int to_move = (kNodeSlots - left->count()) /
                      (1 + (insert_position < static_cast<int>(kNodeSlots)));
        to_move = (std::max)(1, to_move);

        if (insert_position - to_move >= node->start() ||
            left->count() + to_move < static_cast<int>(kNodeSlots)) {
          left->rebalance_right_to_left(to_move, node, mutable_allocator());

          assert(node->max_count() - node->count() == to_move);
          insert_position = insert_position - to_move;
          if (insert_position < node->start()) {
            insert_position = insert_position + left->count() + 1;
            node = left;
          }

          assert(node->count() < node->max_count());
          return;
        }
      }
    }

    if (node->position() < parent->finish()) {
      // Try rebalancing with our right sibling.
      node_type *right = parent->child(node->position() + 1);
      assert(right->max_count() == kNodeSlots);
      if (right->count() < kNodeSlots) {
        // We bias rebalancing based on the position being inserted. If we're
        // inserting at the beginning of the left node then we bias rebalancing
        // to fill up the right node.
        int to_move = (static_cast<int>(kNodeSlots) - right->count()) /
                      (1 + (insert_position > node->start()));
        to_move = (std::max)(1, to_move);

        if (insert_position <= node->finish() - to_move ||
            right->count() + to_move < static_cast<int>(kNodeSlots)) {
          node->rebalance_left_to_right(to_move, right, mutable_allocator());

          if (insert_position > node->finish()) {
            insert_position = insert_position - node->count() - 1;
            node = right;
          }

          assert(node->count() < node->max_count());
          return;
        }
      }
    }

    // Rebalancing failed, make sure there is room on the parent node for a new
    // value.
    assert(parent->max_count() == kNodeSlots);
    if (parent->count() == kNodeSlots) {
      iterator parent_iter(node->parent(), node->position());
      rebalance_or_split(&parent_iter);
    }
  } else {
    // Rebalancing not possible because this is the root node.
    // Create a new root node and set the current root node as the child of the
    // new root.
    parent = new_internal_node(parent);
    parent->init_child(parent->start(), root());
    mutable_root() = parent;
    // If the former root was a leaf node, then it's now the rightmost node.
    assert(!parent->start_child()->leaf() ||
           parent->start_child() == rightmost_);
  }

  // Split the node.
  node_type *split_node;
  if (node->leaf()) {
    split_node = new_leaf_node(parent);
    node->split(insert_position, split_node, mutable_allocator());
    if (rightmost_ == node) rightmost_ = split_node;
  } else {
    split_node = new_internal_node(parent);
    node->split(insert_position, split_node, mutable_allocator());
  }

  if (insert_position > node->finish()) {
    insert_position = insert_position - node->count() - 1;
    node = split_node;
  }
}

template <typename P>
void btree<P>::merge_nodes(node_type *left, node_type *right) {
  left->merge(right, mutable_allocator());
  if (rightmost_ == right) rightmost_ = left;
}

template <typename P>
bool btree<P>::try_merge_or_rebalance(iterator *iter) {
  node_type *parent = iter->node->parent();
  if (iter->node->position() > parent->start()) {
    // Try merging with our left sibling.
    node_type *left = parent->child(iter->node->position() - 1);
    assert(left->max_count() == kNodeSlots);
    if (1U + left->count() + iter->node->count() <= kNodeSlots) {
      iter->position += 1 + left->count();
      merge_nodes(left, iter->node);
      iter->node = left;
      return true;
    }
  }
  if (iter->node->position() < parent->finish()) {
    // Try merging with our right sibling.
    node_type *right = parent->child(iter->node->position() + 1);
    assert(right->max_count() == kNodeSlots);
    if (1U + iter->node->count() + right->count() <= kNodeSlots) {
      merge_nodes(iter->node, right);
      return true;
    }
    // Try rebalancing with our right sibling. We don't perform rebalancing if
    // we deleted the first element from iter->node and the node is not
    // empty. This is a small optimization for the common pattern of deleting
    // from the front of the tree.
    if (right->count() > kMinNodeValues &&
        (iter->node->count() == 0 || iter->position > iter->node->start())) {
      int to_move = (right->count() - iter->node->count()) / 2;
      to_move = (std::min)(to_move, right->count() - 1);
      iter->node->rebalance_right_to_left(to_move, right, mutable_allocator());
      return false;
    }
  }
  if (iter->node->position() > parent->start()) {
    // Try rebalancing with our left sibling. We don't perform rebalancing if
    // we deleted the last element from iter->node and the node is not
    // empty. This is a small optimization for the common pattern of deleting
    // from the back of the tree.
    node_type *left = parent->child(iter->node->position() - 1);
    if (left->count() > kMinNodeValues &&
        (iter->node->count() == 0 || iter->position < iter->node->finish())) {
      int to_move = (left->count() - iter->node->count()) / 2;
      to_move = (std::min)(to_move, left->count() - 1);
      left->rebalance_left_to_right(to_move, iter->node, mutable_allocator());
      iter->position += to_move;
      return false;
    }
  }
  return false;
}

template <typename P>
void btree<P>::try_shrink() {
  node_type *orig_root = root();
  if (orig_root->count() > 0) {
    return;
  }
  // Deleted the last item on the root node, shrink the height of the tree.
  if (orig_root->leaf()) {
    assert(size() == 0);
    mutable_root() = rightmost_ = EmptyNode();
  } else {
    node_type *child = orig_root->start_child();
    child->make_root();
    mutable_root() = child;
  }
  node_type::clear_and_delete(orig_root, mutable_allocator());
}

template <typename P>
template <typename IterType>
inline IterType btree<P>::internal_last(IterType iter) {
  assert(iter.node != nullptr);
  while (iter.position == iter.node->finish()) {
    iter.position = iter.node->position();
    iter.node = iter.node->parent();
    if (iter.node->leaf()) {
      iter.node = nullptr;
      break;
    }
  }
  return iter;
}

template <typename P>
template <typename... Args>
inline auto btree<P>::internal_emplace(iterator iter, Args &&... args)
    -> iterator {
  if (!iter.node->leaf()) {
    // We can't insert on an internal node. Instead, we'll insert after the
    // previous value which is guaranteed to be on a leaf node.
    --iter;
    ++iter.position;
  }
  const field_type max_count = iter.node->max_count();
  allocator_type *alloc = mutable_allocator();
  if (iter.node->count() == max_count) {
    // Make room in the leaf for the new item.
    if (max_count < kNodeSlots) {
      // Insertion into the root where the root is smaller than the full node
      // size. Simply grow the size of the root node.
      assert(iter.node == root());
      iter.node =
          new_leaf_root_node((std::min<int>)(kNodeSlots, 2 * max_count));
      // Transfer the values from the old root to the new root.
      node_type *old_root = root();
      node_type *new_root = iter.node;
      new_root->transfer_n(old_root->count(), new_root->start(),
                           old_root->start(), old_root, alloc);
      new_root->set_finish(old_root->finish());
      old_root->set_finish(old_root->start());
      node_type::clear_and_delete(old_root, alloc);
      mutable_root() = rightmost_ = new_root;
    } else {
      rebalance_or_split(&iter);
    }
  }
  iter.node->emplace_value(iter.position, alloc, std::forward<Args>(args)...);
  ++size_;
  return iter;
}

template <typename P>
template <typename K>
inline auto btree<P>::internal_locate(const K &key) const
    -> SearchResult<iterator, is_key_compare_to::value> {
  iterator iter(const_cast<node_type *>(root()));
  for (;;) {
    SearchResult<int, is_key_compare_to::value> res =
        iter.node->lower_bound(key, key_comp());
    iter.position = res.value;
    if (res.IsEq()) {
      return {iter, MatchKind::kEq};
    }
    // Note: in the non-key-compare-to case, we don't need to walk all the way
    // down the tree if the keys are equal, but determining equality would
    // require doing an extra comparison on each node on the way down, and we
    // will need to go all the way to the leaf node in the expected case.
    if (iter.node->leaf()) {
      break;
    }
    iter.node = iter.node->child(iter.position);
  }
  // Note: in the non-key-compare-to case, the key may actually be equivalent
  // here (and the MatchKind::kNe is ignored).
  return {iter, MatchKind::kNe};
}

template <typename P>
template <typename K>
auto btree<P>::internal_lower_bound(const K &key) const
    -> SearchResult<iterator, is_key_compare_to::value> {
  if (!params_type::template can_have_multiple_equivalent_keys<K>()) {
    SearchResult<iterator, is_key_compare_to::value> ret = internal_locate(key);
    ret.value = internal_last(ret.value);
    return ret;
  }
  iterator iter(const_cast<node_type *>(root()));
  SearchResult<int, is_key_compare_to::value> res;
  bool seen_eq = false;
  for (;;) {
    res = iter.node->lower_bound(key, key_comp());
    iter.position = res.value;
    if (iter.node->leaf()) {
      break;
    }
    seen_eq = seen_eq || res.IsEq();
    iter.node = iter.node->child(iter.position);
  }
  if (res.IsEq()) return {iter, MatchKind::kEq};
  return {internal_last(iter), seen_eq ? MatchKind::kEq : MatchKind::kNe};
}

template <typename P>
template <typename K>
auto btree<P>::internal_upper_bound(const K &key) const -> iterator {
  iterator iter(const_cast<node_type *>(root()));
  for (;;) {
    iter.position = iter.node->upper_bound(key, key_comp());
    if (iter.node->leaf()) {
      break;
    }
    iter.node = iter.node->child(iter.position);
  }
  return internal_last(iter);
}

template <typename P>
template <typename K>
auto btree<P>::internal_find(const K &key) const -> iterator {
  SearchResult<iterator, is_key_compare_to::value> res = internal_locate(key);
  if (res.HasMatch()) {
    if (res.IsEq()) {
      return res.value;
    }
  } else {
    const iterator iter = internal_last(res.value);
    if (iter.node != nullptr && !compare_keys(key, iter.key())) {
      return iter;
    }
  }
  return {nullptr, 0};
}

template <typename P>
int btree<P>::internal_verify(const node_type *node, const key_type *lo,
                              const key_type *hi) const {
  assert(node->count() > 0);
  assert(node->count() <= node->max_count());
  if (lo) {
    assert(!compare_keys(node->key(node->start()), *lo));
  }
  if (hi) {
    assert(!compare_keys(*hi, node->key(node->finish() - 1)));
  }
  for (int i = node->start() + 1; i < node->finish(); ++i) {
    assert(!compare_keys(node->key(i), node->key(i - 1)));
  }
  int count = node->count();
  if (!node->leaf()) {
    for (int i = node->start(); i <= node->finish(); ++i) {
      assert(node->child(i) != nullptr);
      assert(node->child(i)->parent() == node);
      assert(node->child(i)->position() == i);
      count += internal_verify(node->child(i),
                               i == node->start() ? lo : &node->key(i - 1),
                               i == node->finish() ? hi : &node->key(i));
    }
  }
  return count;
}

}  // namespace container_internal
ABSL_NAMESPACE_END
}  // namespace absl

#endif  // ABSL_CONTAINER_INTERNAL_BTREE_H_
