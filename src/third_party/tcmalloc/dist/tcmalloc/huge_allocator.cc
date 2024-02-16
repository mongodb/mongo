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

#include "tcmalloc/huge_allocator.h"

#include <string.h>

#include "tcmalloc/huge_address_map.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

void HugeAllocator::Print(Printer* out) {
  out->printf("HugeAllocator: contiguous, unbacked hugepage(s)\n");
  free_.Print(out);
  out->printf(
      "HugeAllocator: %zu requested - %zu in use = %zu hugepages free\n",
      from_system_.raw_num(), in_use_.raw_num(),
      (from_system_ - in_use_).raw_num());
}

void HugeAllocator::PrintInPbtxt(PbtxtRegion* hpaa) const {
  free_.PrintInPbtxt(hpaa);
  hpaa->PrintI64("num_total_requested_huge_pages", from_system_.raw_num());
  hpaa->PrintI64("num_in_use_huge_pages", in_use_.raw_num());
}

HugeAddressMap::Node* HugeAllocator::Find(HugeLength n) {
  HugeAddressMap::Node* curr = free_.root();
  // invariant: curr != nullptr && curr->longest >= n
  // we favor smaller gaps and lower nodes and lower addresses, in that
  // order. The net effect is that we are neither a best-fit nor a
  // lowest-address allocator but vaguely close to both.
  HugeAddressMap::Node* best = nullptr;
  while (curr && curr->longest() >= n) {
    if (curr->range().len() >= n) {
      if (!best || best->range().len() > curr->range().len()) {
        best = curr;
      }
    }

    // Either subtree could contain a better fit and we don't want to
    // search the whole tree. Pick a reasonable child to look at.
    auto left = curr->left();
    auto right = curr->right();
    if (!left || left->longest() < n) {
      curr = right;
      continue;
    }

    if (!right || right->longest() < n) {
      curr = left;
      continue;
    }

    // Here, we have a nontrivial choice.
    if (left->range().len() == right->range().len()) {
      if (left->longest() <= right->longest()) {
        curr = left;
      } else {
        curr = right;
      }
    } else if (left->range().len() < right->range().len()) {
      // Here, the longest range in both children is the same...look
      // in the subtree with the smaller root, as that's slightly
      // more likely to be our best.
      curr = left;
    } else {
      curr = right;
    }
  }
  return best;
}

void HugeAllocator::CheckFreelist() {
  free_.Check();
  size_t num_nodes = free_.nranges();
  HugeLength n = free_.total_mapped();
  free_.Check();
  CHECK_CONDITION(n == from_system_ - in_use_);
  LargeSpanStats large;
  AddSpanStats(nullptr, &large, nullptr);
  CHECK_CONDITION(num_nodes == large.spans);
  CHECK_CONDITION(n.in_pages() == large.returned_pages);
}

HugeRange HugeAllocator::AllocateRange(HugeLength n) {
  if (n.overflows()) return HugeRange::Nil();
  size_t bytes = n.in_bytes();
  size_t align = kHugePageSize;
  auto [ptr, actual] = allocate_(bytes, align);
  if (ptr == nullptr) {
    // OOM...
    return HugeRange::Nil();
  }
  CHECK_CONDITION(ptr != nullptr);
  // It's possible for a request to return extra hugepages.
  CHECK_CONDITION(actual % kHugePageSize == 0);
  n = HLFromBytes(actual);
  from_system_ += n;
  return HugeRange::Make(HugePageContaining(ptr), n);
}

HugeRange HugeAllocator::Get(HugeLength n) {
  CHECK_CONDITION(n > NHugePages(0));
  auto* node = Find(n);
  if (!node) {
    // Get more memory, then "delete" it
    HugeRange r = AllocateRange(n);
    if (!r.valid()) return r;
    in_use_ += r.len();
    Release(r);
    node = Find(n);
    CHECK_CONDITION(node != nullptr);
  }
  in_use_ += n;

  HugeRange r = node->range();
  free_.Remove(node);
  if (r.len() > n) {
    HugeLength before = r.len();
    HugeRange extra = HugeRange::Make(r.start() + n, before - n);
    r = HugeRange::Make(r.start(), n);
    ASSERT(r.precedes(extra));
    ASSERT(r.len() + extra.len() == before);
    in_use_ += extra.len();
    Release(extra);
  } else {
    // Release does this for us
    DebugCheckFreelist();
  }

  return r;
}

void HugeAllocator::Release(HugeRange r) {
  in_use_ -= r.len();

  free_.Insert(r);
  DebugCheckFreelist();
}

void HugeAllocator::AddSpanStats(SmallSpanStats* small, LargeSpanStats* large,
                                 PageAgeHistograms* ages) const {
  for (const HugeAddressMap::Node* node = free_.first(); node != nullptr;
       node = node->next()) {
    HugeLength n = node->range().len();
    if (large != nullptr) {
      large->spans++;
      large->returned_pages += n.in_pages();
    }

    if (ages != nullptr) {
      ages->RecordRange(n.in_pages(), true, node->when());
    }
  }
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
