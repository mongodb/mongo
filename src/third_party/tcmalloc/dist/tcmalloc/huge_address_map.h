// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_HUGE_ADDRESS_MAP_H_
#define TCMALLOC_HUGE_ADDRESS_MAP_H_
#include <stddef.h>
#include <stdint.h>

#include "absl/base/attributes.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/metadata_allocator.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Maintains a set of disjoint HugeRanges, merging adjacent ranges into one.
// Exposes a balanced (somehow) binary tree of free ranges on address,
// augmented with the largest range in each subtree (this allows fairly simple
// allocation algorithms from the contained ranges.
//
// This class scales well and is *reasonably* performant, but it is not intended
// for use on extremely hot paths.
class HugeAddressMap {
 public:
  explicit constexpr HugeAddressMap(
      MetadataAllocator& meta ABSL_ATTRIBUTE_LIFETIME_BOUND);

  // IMPORTANT: DESTROYING A HUGE ADDRESS MAP DOES NOT MAKE ANY ATTEMPT
  // AT FREEING ALLOCATED METADATA.
  ~HugeAddressMap() = default;

  class Node {
   public:
    // the range stored at this point
    HugeRange range() const;
    // Tree structure
    Node* left();
    Node* right();
    // Iterate to the next node in address order
    const Node* next() const;
    Node* next();
    // when were this node's content added (in
    // absl::base_internal::CycleClock::Now units)?
    int64_t when() const;

    // What is the length of the longest range in the subtree rooted here?
    HugeLength longest() const;

   private:
    Node(HugeRange r, int prio);
    friend class HugeAddressMap;
    HugeRange range_;
    int prio_;  // chosen randomly
    Node *left_, *right_;
    Node* parent_;
    HugeLength longest_;
    int64_t when_;
    // Expensive, recursive consistency check.
    // Accumulates node count and range sizes into passed arguments.
    void Check(size_t* num_nodes, HugeLength* size) const;

    // We've broken longest invariants somehow; fix them here.
    void FixLongest();
  };

  // Get root of the tree.
  Node* root();
  const Node* root() const;

  // Get lowest-addressed node
  const Node* first() const;
  Node* first();

  // Returns the highest-addressed range that does not lie completely
  // after p (if any).
  Node* Predecessor(HugePage p);

  // Expensive consistency check.
  void Check();

  // Statistics
  size_t nranges() const;
  HugeLength total_mapped() const;
  void Print(Printer* out) const;
  void PrintInPbtxt(PbtxtRegion* hpaa) const;

  // Add <r> to the map, merging with adjacent ranges as needed.
  void Insert(HugeRange r);

  // Delete n from the map.
  void Remove(Node* n);

 private:
  // our tree
  Node* root_{nullptr};
  size_t used_nodes_{0};
  HugeLength total_size_{NHugePages(0)};

  // cache of unused nodes
  Node* freelist_{nullptr};
  size_t freelist_size_{0};
  // How we get more
  MetadataAllocator& meta_;
  Node* Get(HugeRange r);
  void Put(Node* n);

  size_t total_nodes_{0};

  void Merge(Node* b, HugeRange r, Node* a);
  void FixLongest(Node* n);
  // Note that we always use the same seed, currently; this isn't very random.
  // In practice we're not worried about adversarial input and this works well
  // enough.
  unsigned int seed_{0};
};

inline constexpr HugeAddressMap::HugeAddressMap(MetadataAllocator& meta)
    : meta_(meta) {}

inline HugeRange HugeAddressMap::Node::range() const { return range_; }
inline HugeAddressMap::Node* HugeAddressMap::Node::left() { return left_; }
inline HugeAddressMap::Node* HugeAddressMap::Node::right() { return right_; }

inline int64_t HugeAddressMap::Node::when() const { return when_; }
inline HugeLength HugeAddressMap::Node::longest() const { return longest_; }

inline HugeAddressMap::Node* HugeAddressMap::root() { return root_; }
inline const HugeAddressMap::Node* HugeAddressMap::root() const {
  return root_;
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HUGE_ADDRESS_MAP_H_
