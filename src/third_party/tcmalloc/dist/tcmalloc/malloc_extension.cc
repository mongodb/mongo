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
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

#include "absl/base/attributes.h"
#include "absl/base/internal/low_level_alloc.h"
#include "absl/memory/memory.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/internal_malloc_extension.h"

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
  static auto* arena =
      absl::base_internal::LowLevelAlloc::NewArena(/*flags=*/0);
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
  std::string ret;
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_GetStats != nullptr) {
    MallocExtension_Internal_GetStats(&ret);
  }
#endif
  return ret;
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
}

void MallocExtension::MarkThreadBusy() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_MarkThreadBusy == nullptr) {
    return;
  }

  MallocExtension_Internal_MarkThreadBusy();
#endif
}

MallocExtension::MemoryLimit MallocExtension::GetMemoryLimit() {
  MemoryLimit ret;
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_GetMemoryLimit != nullptr) {
    MallocExtension_Internal_GetMemoryLimit(&ret);
  }
#endif
  return ret;
}

void MallocExtension::SetMemoryLimit(
    const MallocExtension::MemoryLimit& limit) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_SetMemoryLimit != nullptr) {
    MallocExtension_Internal_SetMemoryLimit(&limit);
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

// TODO(b/263387812): remove when experimentation is complete
bool MallocExtension::GetImprovedGuardedSampling() {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetImprovedGuardedSampling == nullptr) {
    return -1;
  }

  return MallocExtension_Internal_GetImprovedGuardedSampling();
#else
  return false;
#endif
}

// TODO(b/263387812): remove when experimentation is complete
void MallocExtension::SetImprovedGuardedSampling(bool enable) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_SetImprovedGuardedSampling == nullptr) {
    return;
  }

  MallocExtension_Internal_SetImprovedGuardedSampling(enable);
#else
  (void)enable;
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

absl::optional<size_t> MallocExtension::GetNumericProperty(
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
  return absl::nullopt;
}

size_t MallocExtension::GetEstimatedAllocatedSize(size_t size) {
  return nallocx(size, 0);
}

absl::optional<size_t> MallocExtension::GetAllocatedSize(const void* p) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetAllocatedSize != nullptr) {
    return MallocExtension_Internal_GetAllocatedSize(p);
  }
#endif
  return absl::nullopt;
}

MallocExtension::Ownership MallocExtension::GetOwnership(const void* p) {
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (MallocExtension_Internal_GetOwnership != nullptr) {
    return MallocExtension_Internal_GetOwnership(p);
  }
#endif
  return MallocExtension::Ownership::kUnknown;
}

std::map<std::string, MallocExtension::Property>
MallocExtension::GetProperties() {
  std::map<std::string, MallocExtension::Property> ret;
#if ABSL_INTERNAL_HAVE_WEAK_MALLOCEXTENSION_STUBS
  if (&MallocExtension_Internal_GetProperties != nullptr) {
    MallocExtension_Internal_GetProperties(&ret);
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
