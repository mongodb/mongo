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
//
// This file defines policies used when allocation memory.
//
// An allocation policy encapsulates four policies:
//
// - Size returning policy
//   Manages the return type and contents of returned pointer values.
//
//   struct SizeReturningPolicyTemplate {
//     // The type to return. E.g: void* or size_ptr_t.
//     using pointer_type = <pointer type>;
//
//     // Returns true if this policy includes sizes information.
//     static constexpr bool size_returning();
//
//     // Returns a pointer from the provide raw pointer and size information.
//     static pointer_type as_pointer(void* ptr, size_t capacity);
//
//     // Returns a pointer based on the provide raw pointer and size class.
//     static pointer_type to_pointer(void* ptr, size_t size_class);
//   };
//
// - Out of memory policy.
//   Dictates how to handle OOM conditions.
//
//   struct OomPolicyTemplate {
//     // Invoked when we failed to allocate memory.
//     // This method is templated on a size returning policy documented above.
//     // Must either terminate, throw, or return nullptr.
//     template <typename Policy>
//     static Policy::pointer_type handle_oom(size_t size);
//   };
//
// - Alignment policy
//   Dictates alignment to use for an allocation.
//   Must be trivially copyable.
//
//   struct AlignPolicyTemplate {
//     // Returns the alignment to use for the memory allocation,
//     // or 1 to use small allocation table alignments (8 bytes)
//     // Returned value Must be a non-zero power of 2.
//     size_t align() const;
//   };
//
// - Hook invocation policy
//   dictates invocation of allocation hooks
//
//   struct HooksPolicyTemplate {
//     // Returns true if allocation hooks must be invoked.
//     static bool invoke_hooks();
//   };
//
// - NUMA partition policy
//   When NUMA awareness is enabled this dictates which NUMA partition we will
//   allocate memory from. Must be trivially copyable.
//
//   struct NumaPartitionPolicyTemplate {
//     // Returns the NUMA partition to allocate from.
//     size_t partition() const;
//
//     // Returns the NUMA partition to allocate from multiplied by
//     // kNumBaseClasses - i.e. the first size class that corresponds to the
//     // NUMA partition to allocate from.
//     size_t scaled_partition() const;
//   };

#ifndef TCMALLOC_TCMALLOC_POLICY_H_
#define TCMALLOC_TCMALLOC_POLICY_H_

#include <errno.h>
#include <stddef.h>

#include <cstddef>
#include <new>

#include "absl/base/attributes.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// NullOomPolicy: returns nullptr
struct NullOomPolicy {
  template <typename Policy, typename Pointer = typename Policy::pointer_type>
  static inline constexpr Pointer handle_oom(size_t size) {
    return Policy::as_pointer(nullptr, 0);
  }
};

// MallocOomPolicy: sets errno to ENOMEM and returns nullptr
struct MallocOomPolicy {
  template <typename Policy, typename Pointer = typename Policy::pointer_type>
  static inline Pointer handle_oom(size_t size) {
    errno = ENOMEM;
    return Policy::as_pointer(nullptr, 0);
  }
};

// CppOomPolicy: terminates the program
struct CppOomPolicy {
  template <typename Policy, typename Pointer = typename Policy::pointer_type>
  static ABSL_ATTRIBUTE_NOINLINE ABSL_ATTRIBUTE_NORETURN Pointer
  handle_oom(size_t size) {
    CrashWithOOM(size);
  }
};

// DefaultAlignPolicy: use default small size table based allocation
struct DefaultAlignPolicy {
  // Important: the value here is explicitly '1' to indicate that the used
  // alignment is the default alignment of the size tables in tcmalloc.
  // The constexpr value of 1 will optimize out the alignment checks and
  // iterations in the GetSizeClass() calls for default aligned allocations.
  static constexpr size_t align() { return 1; }
};

// MallocAlignPolicy: use std::max_align_t allocation
struct MallocAlignPolicy {
  static constexpr size_t align() { return alignof(std::max_align_t); }
};

// AlignAsPolicy: use user provided alignment
class AlignAsPolicy {
 public:
  AlignAsPolicy() = delete;
  explicit constexpr AlignAsPolicy(size_t value) : value_(value) {}
  explicit constexpr AlignAsPolicy(std::align_val_t value)
      : AlignAsPolicy(static_cast<size_t>(value)) {}

  size_t constexpr align() const { return value_; }

 private:
  size_t value_;
};

// AllocationAccessAsPolicy: use user provided access hint
class AllocationAccessAsPolicy {
 public:
  AllocationAccessAsPolicy() = delete;
  explicit constexpr AllocationAccessAsPolicy(hot_cold_t value)
      : value_(value) {}

  constexpr hot_cold_t access() const { return value_; }

 private:
  hot_cold_t value_;
};

struct AllocationAccessHotPolicy {
  // Important: the value here is explicitly hot_cold_t{255} to allow the value
  // to be constant propagated.  This allows allocations without a hot/cold hint
  // to use the normal fast path.
  static constexpr hot_cold_t access() { return hot_cold_t{255}; }
};

struct AllocationAccessColdPolicy {
  static constexpr hot_cold_t access() { return hot_cold_t{0}; }
};

using DefaultAllocationAccessPolicy = AllocationAccessHotPolicy;

// InvokeHooksPolicy: invoke memory allocation hooks
struct InvokeHooksPolicy {
  static constexpr bool invoke_hooks() { return true; }
};

// NoHooksPolicy: do not invoke memory allocation hooks
struct NoHooksPolicy {
  static constexpr bool invoke_hooks() { return false; }
};

// IsSizeReturningPolicy: Allocation returns size externally
struct IsSizeReturningPolicy {
  using pointer_type = sized_ptr_t;

  static constexpr bool size_returning() { return true; }

  static constexpr pointer_type as_pointer(void* ptr, size_t capacity) {
    return {ptr, capacity};
  }

  static pointer_type to_pointer(void* ptr, size_t size_class) {
    return {ptr, tc_globals.sizemap().class_to_size(size_class)};
  }
};

// NonSizeReturningPolicy: Allocation does not return size externally
struct NonSizeReturningPolicy {
  using pointer_type = void*;

  static constexpr bool size_returning() { return false; }

  static constexpr pointer_type as_pointer(void* ptr, size_t) { return ptr; }

  static pointer_type to_pointer(void* ptr, size_t) { return ptr; }
};

// Use a fixed NUMA partition.
class FixedNumaPartitionPolicy {
 public:
  explicit constexpr FixedNumaPartitionPolicy(size_t partition)
      : partition_(partition) {}

  size_t constexpr partition() const { return partition_; }

  size_t constexpr scaled_partition() const {
    return partition_ * kNumBaseClasses;
  }

 private:
  size_t partition_;
};

// Use the NUMA partition which the executing CPU is local to.
struct LocalNumaPartitionPolicy {
  // Note that the partition returned may change between calls if the executing
  // thread migrates between NUMA nodes & partitions. Users of this function
  // should not rely upon multiple invocations returning the same partition.
  size_t partition() const {
    return tc_globals.numa_topology().GetCurrentPartition();
  }
  size_t scaled_partition() const {
    return tc_globals.numa_topology().GetCurrentScaledPartition();
  }
};

// TCMallocPolicy defines the compound policy object containing
// the OOM, alignment and hooks policies.
// Is trivially constructible, copyable and destructible.
template <typename OomPolicy = CppOomPolicy,
          typename AlignPolicy = DefaultAlignPolicy,
          typename AccessPolicy = DefaultAllocationAccessPolicy,
          typename HooksPolicy = InvokeHooksPolicy,
          typename SizeReturningPolicy = NonSizeReturningPolicy,
          typename NumaPolicy = LocalNumaPartitionPolicy>
class TCMallocPolicy {
 public:
  // Size returning / pointer type
  using pointer_type = typename SizeReturningPolicy::pointer_type;

  constexpr TCMallocPolicy() = default;
  explicit constexpr TCMallocPolicy(AlignPolicy align, NumaPolicy numa)
      : align_(align), numa_(numa) {}
  explicit constexpr TCMallocPolicy(AlignPolicy align, hot_cold_t access,
                                    NumaPolicy numa)
      : align_(align), access_(access), numa_(numa) {}

  // OOM policy
  static pointer_type handle_oom(size_t size) {
    return OomPolicy::template handle_oom<SizeReturningPolicy>(size);
  }

  // Alignment policy
  constexpr size_t align() const { return align_.align(); }

  // NUMA partition
  constexpr size_t numa_partition() const { return numa_.partition(); }

  // NUMA partition multiplied by kNumBaseClasses
  constexpr size_t scaled_numa_partition() const {
    return numa_.scaled_partition();
  }

  constexpr hot_cold_t access() const { return access_.access(); }

  // Hooks policy
  static constexpr bool invoke_hooks() { return HooksPolicy::invoke_hooks(); }

  // Size returning functions
  static constexpr bool size_returning() {
    return SizeReturningPolicy::size_returning();
  }
  static pointer_type as_pointer(void* ptr, size_t capacity) {
    return SizeReturningPolicy::as_pointer(ptr, capacity);
  }
  static pointer_type to_pointer(void* ptr, size_t size_class) {
    return SizeReturningPolicy::to_pointer(ptr, size_class);
  }

  // Returns this policy aligned as 'align'
  template <typename align_t>
  constexpr TCMallocPolicy<OomPolicy, AlignAsPolicy, AccessPolicy, HooksPolicy,
                           SizeReturningPolicy, NumaPolicy>
  AlignAs(align_t align) const {
    return TCMallocPolicy<OomPolicy, AlignAsPolicy, AccessPolicy, HooksPolicy,
                          SizeReturningPolicy, NumaPolicy>(AlignAsPolicy{align},
                                                           numa_);
  }

  // Returns this policy for frequent access
  constexpr TCMallocPolicy<OomPolicy, AlignPolicy, AllocationAccessHotPolicy,
                           HooksPolicy, SizeReturningPolicy, NumaPolicy>
  AccessAsHot() const {
    return TCMallocPolicy<OomPolicy, AlignPolicy, AllocationAccessHotPolicy,
                          HooksPolicy, SizeReturningPolicy, NumaPolicy>(align_,
                                                                        numa_);
  }

  // Returns this policy for infrequent access
  constexpr TCMallocPolicy<OomPolicy, AlignPolicy, AllocationAccessColdPolicy,
                           HooksPolicy, SizeReturningPolicy, NumaPolicy>
  AccessAsCold() const {
    return TCMallocPolicy<OomPolicy, AlignPolicy, AllocationAccessColdPolicy,
                          HooksPolicy, SizeReturningPolicy, NumaPolicy>(align_,
                                                                        numa_);
  }

  // Returns this policy with a nullptr OOM policy.
  constexpr TCMallocPolicy<NullOomPolicy, AlignPolicy, AccessPolicy,
                           HooksPolicy, SizeReturningPolicy, NumaPolicy>
  Nothrow() const {
    return TCMallocPolicy<NullOomPolicy, AlignPolicy, AccessPolicy, HooksPolicy,
                          SizeReturningPolicy, NumaPolicy>(align_, numa_);
  }

  // Returns this policy with NewAllocHook invocations disabled.
  constexpr TCMallocPolicy<OomPolicy, AlignPolicy, AccessPolicy, NoHooksPolicy,
                           SizeReturningPolicy, NumaPolicy>
  WithoutHooks() const {
    return TCMallocPolicy<OomPolicy, AlignPolicy, AccessPolicy, NoHooksPolicy,
                          SizeReturningPolicy, NumaPolicy>(align_, numa_);
  }

  constexpr TCMallocPolicy<OomPolicy, AlignPolicy, AccessPolicy, HooksPolicy,
                           IsSizeReturningPolicy, NumaPolicy>
  SizeReturning() const {
    return TCMallocPolicy<OomPolicy, AlignPolicy, AccessPolicy, HooksPolicy,
                          IsSizeReturningPolicy, NumaPolicy>(align_, numa_);
  }

  // Returns this policy with a fixed NUMA partition.
  constexpr TCMallocPolicy<OomPolicy, AlignPolicy, AccessPolicy, NoHooksPolicy,
                           SizeReturningPolicy, FixedNumaPartitionPolicy>
  InNumaPartition(size_t partition) const {
    return TCMallocPolicy<OomPolicy, AlignPolicy, AccessPolicy, NoHooksPolicy,
                          SizeReturningPolicy, FixedNumaPartitionPolicy>(
        align_, FixedNumaPartitionPolicy{partition});
  }

  // Returns this policy with a fixed NUMA partition matching that of the
  // previously allocated `ptr`.
  constexpr auto InSameNumaPartitionAs(void* ptr) const {
    return InNumaPartition(NumaPartitionFromPointer(ptr));
  }

 private:
  ABSL_ATTRIBUTE_NO_UNIQUE_ADDRESS AlignPolicy align_;
  ABSL_ATTRIBUTE_NO_UNIQUE_ADDRESS AccessPolicy access_;
  ABSL_ATTRIBUTE_NO_UNIQUE_ADDRESS NumaPolicy numa_;
};

using CppPolicy = TCMallocPolicy<CppOomPolicy, DefaultAlignPolicy>;
using MallocPolicy = TCMallocPolicy<MallocOomPolicy, MallocAlignPolicy>;

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_TCMALLOC_POLICY_H_
