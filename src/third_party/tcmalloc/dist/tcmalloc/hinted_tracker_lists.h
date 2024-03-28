// Copyright 2022 The TCMalloc Authors
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

#ifndef TCMALLOC_HINTED_TRACKER_LISTS_H_
#define TCMALLOC_HINTED_TRACKER_LISTS_H_

#include <cstddef>

#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linked_list.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/range_tracker.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// This class wraps an array of N TrackerLists and a Bitmap storing which
// elements are non-empty.
template <class TrackerType, size_t N>
class HintedTrackerLists {
 public:
  using TrackerList = TList<TrackerType>;

  constexpr HintedTrackerLists() : size_{} {}

  // Removes a TrackerType from the first non-empty freelist with index at
  // least n and returns it. Returns nullptr if there is none.
  TrackerType* GetLeast(const size_t n) {
    TC_ASSERT_LT(n, N);
    size_t i = nonempty_.FindSet(n);
    if (i == N) {
      return nullptr;
    }
    TC_ASSERT(!lists_[i].empty());
    TrackerType* pt = lists_[i].first();
    if (lists_[i].remove(pt)) {
      nonempty_.ClearBit(i);
    }
    --size_;
    return pt;
  }

  // Returns a pointer to the TrackerType from the first non-empty freelist with
  // index at least n and returns it. Returns nullptr if there is none.
  //
  // Unlike GetLeast, this does not remove the pointer from the list when it is
  // found.
  TrackerType* PeekLeast(const size_t n) {
    TC_ASSERT_LT(n, N);
    size_t i = nonempty_.FindSet(n);
    if (i == N) {
      return nullptr;
    }
    TC_ASSERT(!lists_[i].empty());
    return lists_[i].first();
  }

  // Adds pointer <pt> to the nonempty_[i] list.
  // REQUIRES: i < N && pt != nullptr.
  void Add(TrackerType* pt, const size_t i) {
    TC_ASSERT_LT(i, N);
    TC_ASSERT_NE(pt, nullptr);
    lists_[i].prepend(pt);
    ++size_;
    nonempty_.SetBit(i);
  }

  // Removes pointer <pt> from the nonempty_[i] list.
  // REQUIRES: i < N && pt != nullptr.
  void Remove(TrackerType* pt, const size_t i) {
    TC_ASSERT_LT(i, N);
    TC_ASSERT_NE(pt, nullptr);
    if (lists_[i].remove(pt)) {
      nonempty_.ClearBit(i);
    }
    --size_;
  }
  const TrackerList& operator[](const size_t n) const {
    TC_ASSERT_LT(n, N);
    return lists_[n];
  }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  // Returns length of the list at an index <n>.
  // REQUIRES: n < N.
  size_t SizeOfList(const size_t n) const {
    TC_ASSERT_LT(n, N);
    return lists_[n].length();
  }
  // Runs a functor on all pointers in the TrackerLists.
  // This method is const but the Functor gets passed a non-const pointer.
  // This quirk is inherited from TrackerList.
  template <typename Functor>
  void Iter(const Functor& func, size_t start) const {
    size_t i = nonempty_.FindSet(start);
    while (i < N) {
      auto& list = lists_[i];
      TC_ASSERT(!list.empty());
      for (TrackerType* pt : list) {
        func(pt);
      }
      i++;
      if (i < N) i = nonempty_.FindSet(i);
    }
  }

 private:
  TrackerList lists_[N];
  size_t size_;
  Bitmap<N> nonempty_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_HINTED_TRACKER_LISTS_H_
