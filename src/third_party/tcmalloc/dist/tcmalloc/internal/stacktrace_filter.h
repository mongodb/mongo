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

#ifndef TCMALLOC_INTERNAL_STACKTRACE_FILTER_H_
#define TCMALLOC_INTERNAL_STACKTRACE_FILTER_H_

#include <atomic>
#include <cstddef>

#include "absl/hash/hash.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/logging.h"

namespace tcmalloc {

class TcMallocTest;
class GuardedAllocAlignmentTest;

namespace tcmalloc_internal {

// This class maintains a small collection of StackTrace hashes which are used
// to inform the selection of allocations to be guarded. It provides two
// functions:
//    - Count: returns the number of times the location has
//      been Add-ed (represents guards placed on allocation from stack trace).
//    - Add: which adds the provided StackTrace to the filter, for use when
//      responding to subsequent Count calls.
// Based on the collection size (kSize), it uses the lower bits (kMask) of the
// StackTrace hash as an index into stack_hashes_with_count_.  It stores a count
// of the number of times a hash has been 'Add'-ed in the lower bits (kMask).
class StackTraceFilter {
 public:
  constexpr StackTraceFilter() = default;

  size_t Count(const StackTrace& stacktrace) const;
  void Add(const StackTrace& stacktrace);
  size_t max_slots_used() const {
    return max_slots_used_.load(std::memory_order_relaxed);
  }
  size_t replacement_inserts() const {
    return replacement_inserts_.load(std::memory_order_relaxed);
  }

  // For Testing Only: expunge all counts, allowing for resetting the count,
  // which allows the Improved Coverage algorithm to guard a specific stack
  // trace more than kMaxGuardsPerStackTraceSignature times.
  void Reset();

 private:
  constexpr static size_t kMask = 0xFF;
  constexpr static size_t kHashCountLimit = kMask;
  constexpr static int kSize = kMask + 1;
  std::atomic<size_t> stack_hashes_with_count_[kSize]{0};
  std::atomic<size_t> max_slots_used_{0};
  std::atomic<size_t> replacement_inserts_{0};

  inline size_t HashOfStackTrace(const StackTrace& stacktrace) const {
    return absl::HashOf(
        absl::Span<void* const>(stacktrace.stack, stacktrace.depth));
  }

  friend class GuardedPageAllocatorProfileTest;
  friend class StackTraceFilterTest;
  friend class StackTraceFilterThreadedTest;
  friend class tcmalloc::TcMallocTest;
  friend class tcmalloc::GuardedAllocAlignmentTest;
};

inline size_t StackTraceFilter::Count(const StackTrace& stacktrace) const {
  size_t stack_hash = HashOfStackTrace(stacktrace);
  size_t existing_stack_hash_with_count =
      stack_hashes_with_count_[stack_hash % kSize].load(
          std::memory_order_relaxed);
  //  New stack trace
  if (existing_stack_hash_with_count == 0) {
    return 0;
  }
  // Different stack trace, treat as new
  if ((stack_hash & ~kMask) != (existing_stack_hash_with_count & ~kMask)) {
    return 0;
  }
  // Return a value based on the count of the most frequently guarded stack.
  return existing_stack_hash_with_count & kMask;
}

inline void StackTraceFilter::Add(const StackTrace& stacktrace) {
  size_t stack_hash = HashOfStackTrace(stacktrace);
  size_t existing_stack_hash_with_count =
      stack_hashes_with_count_[stack_hash % kSize].load(
          std::memory_order_relaxed);
  size_t count = 1;
  if ((existing_stack_hash_with_count & ~kMask) == (stack_hash & ~kMask)) {
    // matching entry, increment count
    count = (existing_stack_hash_with_count & kMask) + 1;
    // max count has been reached, skip storing incremented count
    if (count > kHashCountLimit) {
      return;
    }
    stack_hashes_with_count_[stack_hash % kSize].store(
        (stack_hash & ~kMask) | count, std::memory_order_relaxed);
  } else {
    if (existing_stack_hash_with_count == 0) {
      max_slots_used_.fetch_add(1, std::memory_order_relaxed);
    } else {
      replacement_inserts_.fetch_add(1, std::memory_order_relaxed);
    }
    // New stack_hash being placed in (unoccupied entry || existing entry)
    stack_hashes_with_count_[stack_hash % kSize].store(
        (stack_hash & ~kMask) | 1, std::memory_order_relaxed);
  }
}

inline void StackTraceFilter::Reset() {
  for (size_t index = 0; index < kSize; ++index) {
    stack_hashes_with_count_[index].store(0, std::memory_order_relaxed);
  }
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_INTERNAL_STACKTRACE_FILTER_H_
