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

#include "tcmalloc/malloc_extension.h"

#include <assert.h>
#include <string.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/internal/low_level_alloc.h"
#include "absl/functional/function_ref.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "tcmalloc/internal_malloc_extension.h"

#if defined(ABSL_HAVE_ADDRESS_SANITIZER) ||   \
    defined(ABSL_HAVE_MEMORY_SANITIZER) ||    \
    defined(ABSL_HAVE_THREAD_SANITIZER) ||    \
    defined(ABSL_HAVE_HWADDRESS_SANITIZER) || \
    defined(ABSL_HAVE_DATAFLOW_SANITIZER) || defined(ABSL_HAVE_LEAK_SANITIZER)
#if !defined(TCMALLOC_UNDER_SANITIZERS)
#define TCMALLOC_UNDER_SANITIZERS 1
#endif
static constexpr size_t kTerabyte = (size_t)(1ULL << 40);

#include <sanitizer/allocator_interface.h>

#if defined(ABSL_HAVE_ADDRESS_SANITIZER)
static size_t SanitizerVirtualMemoryOverhead() { return 20 * kTerabyte; }

static size_t SanitizerMemoryUsageMultiplier() { return 2; }

static size_t SanitizerStackSizeMultiplier() { return 4; }
#endif

#if defined(ABSL_HAVE_THREAD_SANITIZER)
static size_t SanitizerVirtualMemoryOverhead() { return 98 * kTerabyte; }

static size_t SanitizerMemoryUsageMultiplier() { return 5; }

static size_t SanitizerStackSizeMultiplier() { return 5; }
#endif

#if defined(ABSL_HAVE_MEMORY_SANITIZER)
#include <sanitizer/msan_interface.h>

static size_t SanitizerVirtualMemoryOverhead() {
  return (__msan_get_track_origins() ? 40 : 20) * kTerabyte;
}

static size_t SanitizerMemoryUsageMultiplier() {
  return __msan_get_track_origins() ? 3 : 2;
}

static size_t SanitizerStackSizeMultiplier() {
  // Very rough estimate based on analysing "sub $.*, %rsp" instructions.
  return 2;
}
#endif

#if defined(ABSL_HAVE_HWADDRESS_SANITIZER)
static size_t SanitizerVirtualMemoryOverhead() { return 20 * kTerabyte; }

static size_t SanitizerMemoryUsageMultiplier() { return 1; }

static size_t SanitizerStackSizeMultiplier() { return 1; }
#endif

#if defined(ABSL_HAVE_DATAFLOW_SANITIZER)
#include <sanitizer/dfsan_interface.h>

static size_t SanitizerVirtualMemoryOverhead() { return 40 * kTerabyte; }

static size_t SanitizerMemoryUsageMultiplier() {
  return dfsan_get_track_origins() ? 3 : 2;
}

static size_t SanitizerStackSizeMultiplier() {
  // Very rough estimate based on analysing "sub $.*, %rsp" instructions.
  return dfsan_get_track_origins() ? 3 : 2;
}
#endif

#if defined(ABSL_HAVE_LEAK_SANITIZER) &&     \
    !defined(ABSL_HAVE_ADDRESS_SANITIZER) && \
    !defined(ABSL_HAVE_HWADDRESS_SANITIZER)
static size_t SanitizerVirtualMemoryOverhead() { return 0; }

static size_t SanitizerMemoryUsageMultiplier() { return 1; }

static size_t SanitizerStackSizeMultiplier() { return 1; }
#endif

#else
#define TCMALLOC_UNDER_SANITIZERS 0
#endif

namespace tcmalloc {

MallocExtension::AllocationProfilingToken::AllocationProfilingToken(
    std::unique_ptr<tcmalloc_internal::AllocationProfilingTokenBase> impl)
    : impl_(std::move(impl)) {}

MallocExtension::AllocationProfilingToken::~AllocationProfilingToken() {}

Profile MallocExtension::AllocationProfilingToken::Stop() && {
  std::unique_ptr<tcmalloc_internal::AllocationProfilingTokenBase> p(
      std::move(impl_));
  if (!p) {
    return Profile();
  }
  return std::move(*p).Stop();
}

Profile::Profile(std::unique_ptr<const tcmalloc_internal::ProfileBase> impl)
    : impl_(std::move(impl)) {}

Profile::~Profile() {}

void Profile::Iterate(absl::FunctionRef<void(const Sample&)> f) const {
  if (!impl_) {
    return;
  }

  impl_->Iterate(f);
}

ProfileType Profile::Type() const {
  if (!impl_) {
    return ProfileType::kDoNotUse;
  }

  return impl_->Type();
}

absl::Duration Profile::Duration() const {
  if (!impl_) {
    return absl::ZeroDuration();
  }

  return impl_->Duration();
}

AddressRegion::~AddressRegion() {}

AddressRegionFactory::~AddressRegionFactory() {}

size_t AddressRegionFactory::GetStats(absl::Span<char> buffer) {
  static_cast<void>(buffer);
  return 0;
}

size_t AddressRegionFactory::GetStatsInPbtxt(absl::Span<char> buffer) {
  static_cast<void>(buffer);
  return 0;
}

static std::atomic<size_t> address_region_factory_internal_bytes_allocated(0);

size_t AddressRegionFactory::InternalBytesAllocated() {
  return address_region_factory_internal_bytes_allocated.load(
      std::memory_order_relaxed);
}

void* AddressRegionFactory::MallocInternal(size_t size) {
  // Use arena without malloc hooks to avoid HeapChecker reporting a leak.
  ABSL_CONST_INIT static absl::base_internal::LowLevelAlloc::Arena* arena;
  ABSL_CONST_INIT static absl::once_flag flag;

  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    arena = absl::base_internal::LowLevelAlloc::NewArena(/*flags=*/0);
  });
  void* result =
      absl::base_internal::LowLevelAlloc::AllocWithArena(size, arena);
  if (result) {
    address_region_factory_internal_bytes_allocated.fetch_add(
        size, std::memory_order_relaxed);
  }
  return result;
}

#if !ABSL_HAVE_ATTRIBUTE_WEAK || defined(__APPLE__) || defined(__EMSCRIPTEN__)
#define ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS 0
#else
#define ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS 1
#endif

std::string MallocExtension::GetStats() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_GetStats != nullptr) {
    std::string ret;
    MallocExtension_Internal_GetStats(&ret);
    return ret;
  }
#endif
#if defined(ABSL_HAVE_THREAD_SANITIZER)
  return "NOT IMPLEMENTED";
#endif
  return "";
}

void MallocExtension::ReleaseMemoryToSystem(size_t num_bytes) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_ReleaseMemoryToSystem != nullptr) {
    MallocExtension_Internal_ReleaseMemoryToSystem(num_bytes);
  }
#endif
}

AddressRegionFactory* MallocExtension::GetRegionFactory() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_GetRegionFactory == nullptr) {
    return nullptr;
  }

  return MallocExtension_Internal_GetRegionFactory();
#else
  return nullptr;
#endif
}

void MallocExtension::SetRegionFactory(AddressRegionFactory* factory) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_SetRegionFactory == nullptr) {
    return;
  }

  MallocExtension_Internal_SetRegionFactory(factory);
#endif
  // Default implementation does nothing
}

Profile MallocExtension::SnapshotCurrent(tcmalloc::ProfileType type) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_SnapshotCurrent == nullptr) {
    return Profile();
  }

  return tcmalloc_internal::ProfileAccessor::MakeProfile(
      std::unique_ptr<const tcmalloc_internal::ProfileBase>(
          MallocExtension_Internal_SnapshotCurrent(type)));
#else
  return Profile();
#endif
}

MallocExtension::AllocationProfilingToken
MallocExtension::StartAllocationProfiling() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_StartAllocationProfiling == nullptr) {
    return {};
  }

  return tcmalloc_internal::AllocationProfilingTokenAccessor::MakeToken(
      std::unique_ptr<tcmalloc_internal::AllocationProfilingTokenBase>(
          MallocExtension_Internal_StartAllocationProfiling()));
#else
  return {};
#endif
}

MallocExtension::AllocationProfilingToken
MallocExtension::StartLifetimeProfiling() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_StartLifetimeProfiling == nullptr) {
    return {};
  }

  return tcmalloc_internal::AllocationProfilingTokenAccessor::MakeToken(
      std::unique_ptr<tcmalloc_internal::AllocationProfilingTokenBase>(
          MallocExtension_Internal_StartLifetimeProfiling()));
#else
  return {};
#endif
}

void MallocExtension::MarkThreadIdle() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_MarkThreadIdle == nullptr) {
    return;
  }

  MallocExtension_Internal_MarkThreadIdle();
#endif
  // TODO(b/273799005) -  move __tsan_on_thread_idle call here from
  // testing/tsan/v2/allocator.cconce we have it available in shared tsan
  // libraries.
}

void MallocExtension::MarkThreadBusy() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_MarkThreadBusy == nullptr) {
    return;
  }

  MallocExtension_Internal_MarkThreadBusy();
#endif
}

size_t MallocExtension::GetMemoryLimit(LimitKind limit_kind) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_GetMemoryLimit != nullptr) {
    return MallocExtension_Internal_GetMemoryLimit(limit_kind);
  }
#endif
  return 0;
}

ABSL_INTERNAL_DISABLE_DEPRECATED_DECLARATION_WARNING
MallocExtension::MemoryLimit MallocExtension::GetMemoryLimit() {
  MemoryLimit result;
  const size_t hard_limit = GetMemoryLimit(LimitKind::kHard);
  if (hard_limit != 0 && hard_limit != std::numeric_limits<size_t>::max()) {
    result.limit = hard_limit;
    result.hard = true;
  } else {
    result.limit = GetMemoryLimit(LimitKind::kSoft);
    result.hard = false;
  }
  return result;
}
ABSL_INTERNAL_RESTORE_DEPRECATED_DECLARATION_WARNING

void MallocExtension::SetMemoryLimit(const size_t limit, LimitKind limit_kind) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_SetMemoryLimit != nullptr) {
    // limit == 0 implies no limit.
    const size_t new_limit =
        (limit > 0) ? limit : std::numeric_limits<size_t>::max();
    MallocExtension_Internal_SetMemoryLimit(new_limit, limit_kind);
  }
#endif
}

int64_t MallocExtension::GetProfileSamplingRate() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_GetProfileSamplingRate != nullptr) {
    return MallocExtension_Internal_GetProfileSamplingRate();
  }
#endif
  return -1;
}

void MallocExtension::SetProfileSamplingRate(int64_t rate) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_SetProfileSamplingRate != nullptr) {
    MallocExtension_Internal_SetProfileSamplingRate(rate);
  }
#endif
  (void)rate;
}

int64_t MallocExtension::GetGuardedSamplingRate() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetGuardedSamplingRate == nullptr) {
    return -1;
  }

  return MallocExtension_Internal_GetGuardedSamplingRate();
#else
  return -1;
#endif
}

void MallocExtension::SetGuardedSamplingRate(int64_t rate) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_SetGuardedSamplingRate == nullptr) {
    return;
  }

  MallocExtension_Internal_SetGuardedSamplingRate(rate);
#else
  (void)rate;
#endif
}

void MallocExtension::ActivateGuardedSampling() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_ActivateGuardedSampling != nullptr) {
    MallocExtension_Internal_ActivateGuardedSampling();
  }
#endif
}

bool MallocExtension::PerCpuCachesActive() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetPerCpuCachesActive == nullptr) {
    return false;
  }

  return MallocExtension_Internal_GetPerCpuCachesActive();
#else
  return false;
#endif
}

int32_t MallocExtension::GetMaxPerCpuCacheSize() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetMaxPerCpuCacheSize == nullptr) {
    return -1;
  }

  return MallocExtension_Internal_GetMaxPerCpuCacheSize();
#else
  return -1;
#endif
}

void MallocExtension::SetMaxPerCpuCacheSize(int32_t value) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_SetMaxPerCpuCacheSize == nullptr) {
    return;
  }

  MallocExtension_Internal_SetMaxPerCpuCacheSize(value);
#else
  (void)value;
#endif
}

int64_t MallocExtension::GetMaxTotalThreadCacheBytes() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetMaxTotalThreadCacheBytes == nullptr) {
    return -1;
  }

  return MallocExtension_Internal_GetMaxTotalThreadCacheBytes();
#else
  return -1;
#endif
}

void MallocExtension::SetMaxTotalThreadCacheBytes(int64_t value) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_SetMaxTotalThreadCacheBytes == nullptr) {
    return;
  }

  MallocExtension_Internal_SetMaxTotalThreadCacheBytes(value);
#else
  (void)value;
#endif
}

absl::Duration MallocExtension::GetSkipSubreleaseInterval() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetSkipSubreleaseInterval == nullptr) {
    return absl::ZeroDuration();
  }

  absl::Duration value;
  MallocExtension_Internal_GetSkipSubreleaseInterval(&value);
  return value;
#else
  return absl::ZeroDuration();
#endif
}

void MallocExtension::SetSkipSubreleaseInterval(absl::Duration value) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_SetSkipSubreleaseInterval == nullptr) {
    return;
  }

  MallocExtension_Internal_SetSkipSubreleaseInterval(value);
#else
  (void)value;
#endif
}

bool MallocExtension::GetBackgroundProcessActionsEnabled() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetBackgroundProcessActionsEnabled == nullptr) {
    return false;
  }

  return MallocExtension_Internal_GetBackgroundProcessActionsEnabled();
#else
  return false;
#endif
}

void MallocExtension::SetBackgroundProcessActionsEnabled(bool value) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_SetBackgroundProcessActionsEnabled == nullptr) {
    return;
  }

  MallocExtension_Internal_SetBackgroundProcessActionsEnabled(value);
#else
  (void)value;
#endif
}

absl::Duration MallocExtension::GetBackgroundProcessSleepInterval() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetBackgroundProcessSleepInterval == nullptr) {
    return absl::ZeroDuration();
  }

  absl::Duration value;
  MallocExtension_Internal_GetBackgroundProcessSleepInterval(&value);
  return value;
#else
  return absl::ZeroDuration();
#endif
}

void MallocExtension::SetBackgroundProcessSleepInterval(absl::Duration value) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_SetBackgroundProcessSleepInterval == nullptr) {
    return;
  }

  MallocExtension_Internal_SetBackgroundProcessSleepInterval(value);
#else
  (void)value;
#endif
}

absl::Duration MallocExtension::GetSkipSubreleaseShortInterval() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetSkipSubreleaseShortInterval == nullptr) {
    return absl::ZeroDuration();
  }

  absl::Duration value;
  MallocExtension_Internal_GetSkipSubreleaseShortInterval(&value);
  return value;
#else
  return absl::ZeroDuration();
#endif
}

void MallocExtension::SetSkipSubreleaseShortInterval(absl::Duration value) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_SetSkipSubreleaseShortInterval == nullptr) {
    return;
  }

  MallocExtension_Internal_SetSkipSubreleaseShortInterval(value);
#else
  (void)value;
#endif
}

absl::Duration MallocExtension::GetSkipSubreleaseLongInterval() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetSkipSubreleaseLongInterval == nullptr) {
    return absl::ZeroDuration();
  }

  absl::Duration value;
  MallocExtension_Internal_GetSkipSubreleaseLongInterval(&value);
  return value;
#else
  return absl::ZeroDuration();
#endif
}

void MallocExtension::SetSkipSubreleaseLongInterval(absl::Duration value) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_SetSkipSubreleaseLongInterval == nullptr) {
    return;
  }

  MallocExtension_Internal_SetSkipSubreleaseLongInterval(value);
#else
  (void)value;
#endif
}

std::optional<size_t> MallocExtension::GetNumericProperty(
    absl::string_view property) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_GetNumericProperty != nullptr) {
    size_t value;
    if (MallocExtension_Internal_GetNumericProperty(property.data(),
                                                    property.size(), &value)) {
      return value;
    }
  }
#endif
#if TCMALLOC_UNDER_SANITIZERS
  // TODO(b/273946827): Add local tcmalloc tests for the various sanitizer
  // configs as opposed to depending on
  // //testing/sanitizer_common:malloc_extension_test
  // LINT.IfChange(SanitizerGetProperty)
  if (property == "dynamic_tool.virtual_memory_overhead") {
    return SanitizerVirtualMemoryOverhead();
  }
  if (property == "dynamic_tool.memory_usage_multiplier") {
    return SanitizerMemoryUsageMultiplier();
  }
  if (property == "dynamic_tool.stack_size_multiplier") {
    return SanitizerStackSizeMultiplier();
  }
  if (property == "generic.current_allocated_bytes") {
    return __sanitizer_get_current_allocated_bytes();
  }
  if (property == "generic.heap_size") {
    return __sanitizer_get_heap_size();
  }
  if (property == "tcmalloc.per_cpu_caches_active") {
    // Queried by ReleasePerCpuMemoryToOS().
    return 0;
  }
  if (property == "tcmalloc.pageheap_free_bytes") {
    return __sanitizer_get_free_bytes();
  }
  if (property == "tcmalloc.pageheap_unmapped_bytes") {
    return __sanitizer_get_unmapped_bytes();
  }
  if (property == "tcmalloc.slack_bytes") {
    // Kept for backwards compatibility.
    return __sanitizer_get_free_bytes() + __sanitizer_get_unmapped_bytes();
  }
  // LINT.ThenChange(:SanitizerGetProperties)
#endif  // TCMALLOC_UNDER_SANITIZERS
  return std::nullopt;
}

size_t MallocExtension::GetEstimatedAllocatedSize(size_t size) {
  return nallocx(size, 0);
}

std::optional<size_t> MallocExtension::GetAllocatedSize(const void* p) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetAllocatedSize != nullptr) {
    return MallocExtension_Internal_GetAllocatedSize(p);
  }
#endif
#if TCMALLOC_UNDER_SANITIZERS
  return __sanitizer_get_allocated_size(p);
#endif
  return std::nullopt;
}

MallocExtension::Ownership MallocExtension::GetOwnership(const void* p) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetOwnership != nullptr) {
    return MallocExtension_Internal_GetOwnership(p);
  }
#endif
#if TCMALLOC_UNDER_SANITIZERS
  return __sanitizer_get_ownership(p)
             ? tcmalloc::MallocExtension::Ownership::kOwned
             : tcmalloc::MallocExtension::Ownership::kNotOwned;
#endif
  return MallocExtension::Ownership::kUnknown;
}

std::map<std::string, MallocExtension::Property>
MallocExtension::GetProperties() {
  std::map<std::string, MallocExtension::Property> ret;
#if TCMALLOC_UNDER_SANITIZERS
  // Unlike other extension points this one fills in sanitizer data before the
  // weak function is called so that the weak function can override as needed.
  // LINT.IfChange(SanitizerGetProperties)
  const std::array properties = {"dynamic_tool.virtual_memory_overhead",
                                 "dynamic_tool.memory_usage_multiplier",
                                 "dynamic_tool.stack_size_multiplier",
                                 "generic.current_allocated_bytes",
                                 "generic.heap_size",
                                 "tcmalloc.per_cpu_caches_active",
                                 "tcmalloc.pageheap_free_bytes",
                                 "tcmalloc.pageheap_unmapped_bytes",
                                 "tcmalloc.slack_bytes"};
  // LINT.ThenChange(:SanitizerGetProperty)

  for (const auto& p : properties) {
    const auto& value = GetNumericProperty(p);
    if (value) {
      ret[p].value = *value;
    }
  }
#endif  // TCMALLOC_UNDER_SANITIZERS
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_GetProperties != nullptr) {
    MallocExtension_Internal_GetProperties(&ret);
  }
  if (&MallocExtension_Internal_GetExperiments != nullptr) {
    MallocExtension_Internal_GetExperiments(&ret);
  }
#endif
  return ret;
}

size_t MallocExtension::ReleaseCpuMemory(int cpu) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_ReleaseCpuMemory != nullptr) {
    return MallocExtension_Internal_ReleaseCpuMemory(cpu);
  }
#endif
  return 0;
}

void MallocExtension::ProcessBackgroundActions() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (NeedsProcessBackgroundActions()) {
    MallocExtension_Internal_ProcessBackgroundActions();
  }
#endif
}

bool MallocExtension::NeedsProcessBackgroundActions() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  return &MallocExtension_Internal_ProcessBackgroundActions != nullptr;
#else
  return false;
#endif
}

MallocExtension::BytesPerSecond MallocExtension::GetBackgroundReleaseRate() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_GetBackgroundReleaseRate != nullptr) {
    return MallocExtension_Internal_GetBackgroundReleaseRate();
  }
#endif
  return static_cast<MallocExtension::BytesPerSecond>(0);
}

void MallocExtension::SetBackgroundReleaseRate(BytesPerSecond rate) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_SetBackgroundReleaseRate != nullptr) {
    MallocExtension_Internal_SetBackgroundReleaseRate(rate);
  }
#endif
}

}  // namespace tcmalloc

// Default implementation just returns size. The expectation is that
// the linked-in malloc implementation might provide an override of
// this weak function with a better implementation.
ABSL_ATTRIBUTE_WEAK ABSL_ATTRIBUTE_NOINLINE size_t nallocx(size_t size,
                                                           int) noexcept {
#if TCMALLOC_UNDER_SANITIZERS
  return __sanitizer_get_estimated_allocated_size(size);
#endif
  return size;
}

// Default implementation just frees memory.  The expectation is that the
// linked-in malloc implementation may provide an override with an
// implementation that uses this optimization.
ABSL_ATTRIBUTE_WEAK ABSL_ATTRIBUTE_NOINLINE void sdallocx(void* ptr, size_t,
                                                          int) noexcept {
  free(ptr);
}

ABSL_ATTRIBUTE_WEAK ABSL_ATTRIBUTE_NOINLINE tcmalloc::sized_ptr_t
tcmalloc_size_returning_operator_new(size_t size) {
  return {::operator new(size), size};
}

ABSL_ATTRIBUTE_WEAK ABSL_ATTRIBUTE_NOINLINE tcmalloc::sized_ptr_t
tcmalloc_size_returning_operator_new_hot_cold(size_t size,
                                              tcmalloc::hot_cold_t) {
  return {::operator new(size), size};
}

ABSL_ATTRIBUTE_WEAK ABSL_ATTRIBUTE_NOINLINE tcmalloc::sized_ptr_t
tcmalloc_size_returning_operator_new_nothrow(size_t size) noexcept {
  void* p = ::operator new(size, std::nothrow);
  return {p, p ? size : 0};
}

ABSL_ATTRIBUTE_WEAK ABSL_ATTRIBUTE_NOINLINE tcmalloc::sized_ptr_t
tcmalloc_size_returning_operator_new_hot_cold_nothrow(
    size_t size, tcmalloc::hot_cold_t) noexcept {
  void* p = ::operator new(size, std::nothrow);
  return {p, p ? size : 0};
}

#if defined(_LIBCPP_VERSION) && defined(__cpp_aligned_new)

ABSL_ATTRIBUTE_WEAK ABSL_ATTRIBUTE_NOINLINE tcmalloc::sized_ptr_t
tcmalloc_size_returning_operator_new_aligned(size_t size,
                                             std::align_val_t alignment) {
  return {::operator new(size, alignment), size};
}

ABSL_ATTRIBUTE_WEAK ABSL_ATTRIBUTE_NOINLINE tcmalloc::sized_ptr_t
tcmalloc_size_returning_operator_new_aligned_nothrow(
    size_t size, std::align_val_t alignment) noexcept {
  void* p = ::operator new(size, alignment, std::nothrow);
  return {p, p ? size : 0};
}

ABSL_ATTRIBUTE_WEAK ABSL_ATTRIBUTE_NOINLINE tcmalloc::sized_ptr_t
tcmalloc_size_returning_operator_new_aligned_hot_cold(
    size_t size, std::align_val_t alignment, tcmalloc::hot_cold_t) {
  return {::operator new(size, alignment), size};
}

ABSL_ATTRIBUTE_WEAK ABSL_ATTRIBUTE_NOINLINE tcmalloc::sized_ptr_t
tcmalloc_size_returning_operator_new_aligned_hot_cold_nothrow(
    size_t size, std::align_val_t alignment, tcmalloc::hot_cold_t) noexcept {
  void* p = ::operator new(size, alignment, std::nothrow);
  return {p, p ? size : 0};
}

#endif  // _LIBCPP_VERSION && __cpp_aligned_new
