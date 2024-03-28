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

#ifndef TCMALLOC_ALLOCATION_SAMPLING_H_
#define TCMALLOC_ALLOCATION_SAMPLING_H_

#include <cstddef>
#include <memory>
#include <optional>

#include "absl/base/attributes.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/span.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal {

class Static;

// This function computes a profile that maps a live stack trace to
// the number of bytes of central-cache memory pinned by an allocation
// at that stack trace.
// In the case when span is hosting >= 1 number of small objects (t.proxy !=
// nullptr), we call span::Fragmentation() and read `span->allocated_`. It is
// safe to do so since we hold the per-sample lock while iterating over sampled
// allocations. It prevents the sampled allocation that has the proxy object to
// complete deallocation, thus `proxy` can not be returned to the span yet. It
// thus prevents the central free list to return the span to the page heap.
std::unique_ptr<const ProfileBase> DumpFragmentationProfile(Static& state);

std::unique_ptr<const ProfileBase> DumpHeapProfile(Static& state);

extern "C" ABSL_CONST_INIT thread_local Sampler tcmalloc_sampler
    ABSL_ATTRIBUTE_INITIAL_EXEC;

// Compiler needs to see definition of this variable to generate more
// efficient code for -fPIE/PIC. If the compiler does not see the definition
// it considers it may come from another dynamic library. So even for
// initial-exec model, it need to emit an access via GOT (GOTTPOFF).
// When it sees the definition, it can emit direct %fs:TPOFF access.
// So we provide a weak definition here, but the actual definition is in
// percpu_rseq_asm.S.

/////////////////////////////////////////////////////////////////////////////////
// MONGO HACK
// Remove the weak symbols for the TLS variables from the dynamic builds.
// This will lead to slightly worse code as described above but avoids problems
// where the weak definitions for the TLS variables are preferred over the definitions in
// percpu_rseq_asm.S. We must use the definitions for the TLS variables in
// percpu_rseq_asm.S due to their layout requirements. There are no issues in non-dynamic builds.
//
#ifndef MONGO_TCMALLOC_DYNAMIC_BUILD
ABSL_CONST_INIT ABSL_ATTRIBUTE_WEAK thread_local Sampler tcmalloc_sampler
    ABSL_ATTRIBUTE_INITIAL_EXEC;
#endif

inline Sampler* GetThreadSampler() {
  static_assert(sizeof(Sampler) == TCMALLOC_SAMPLER_SIZE,
                "update TCMALLOC_SAMPLER_SIZE");
  static_assert(alignof(Sampler) == TCMALLOC_SAMPLER_ALIGN,
                "update TCMALLOC_SAMPLER_ALIGN");
  static_assert(Sampler::HotDataOffset() == TCMALLOC_SAMPLER_HOT_OFFSET,
                "update TCMALLOC_SAMPLER_HOT_OFFSET");
  return &tcmalloc_sampler;
}

// Performs sampling for already occurred allocation of object.
//
// For very small object sizes, object is used as 'proxy' and full
// page with sampled marked is allocated instead.
//
// For medium-sized objects that have single instance per span,
// they're simply freed and fresh page span is allocated to represent
// sampling.
//
// For large objects (i.e. allocated with do_malloc_pages) they are
// also fully reused and their span is marked as sampled.
//
// Note that do_free_with_size assumes sampled objects have
// page-aligned addresses. Please change both functions if need to
// invalidate the assumption.
//
// Note that size_class might not match requested_size in case of
// memalign. I.e. when larger than requested allocation is done to
// satisfy alignment constraint.
//
// In case of out-of-memory condition when allocating span or
// stacktrace struct, this function simply cheats and returns original
// object. As if no sampling was requested.
sized_ptr_t SampleifyAllocation(Static& state, size_t requested_size,
                                size_t align, size_t weight, size_t size_class,
                                hot_cold_t access_hint, bool size_returning,
                                void* obj, Span* span);

void MaybeUnsampleAllocation(Static& state, void* ptr,
                             std::optional<size_t> size, Span* span);

template <typename Policy>
static sized_ptr_t SampleLargeAllocation(Static& state, Policy policy,
                                         size_t requested_size, size_t weight,
                                         Span* span) {
  return SampleifyAllocation(state, requested_size, policy.align(), weight, 0,
                             policy.access(), policy.size_returning(), nullptr,
                             span);
}

template <typename Policy>
static sized_ptr_t SampleSmallAllocation(Static& state, Policy policy,
                                         size_t requested_size, size_t weight,
                                         size_t size_class, sized_ptr_t res) {
  return SampleifyAllocation(state, requested_size, policy.align(), weight,
                             size_class, policy.access(),
                             policy.size_returning(), res.p, nullptr);
}
}  // namespace tcmalloc::tcmalloc_internal
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_ALLOCATION_SAMPLING_H_
