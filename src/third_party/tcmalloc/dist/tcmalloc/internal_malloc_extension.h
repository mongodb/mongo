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

// Extra extensions exported by some malloc implementations.  These
// extensions are accessed through a virtual base class so an
// application can link against a malloc that does not implement these
// extensions, and it will get default versions that do nothing.

#ifndef TCMALLOC_INTERNAL_MALLOC_EXTENSION_H_
#define TCMALLOC_INTERNAL_MALLOC_EXTENSION_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/time/time.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace tcmalloc_internal {

// AllocationProfilingTokenAccessor and ProfileAccessor provide access to the
// private constructors of AllocationProfilingToken and Profile that take a
// pointer.
class AllocationProfilingTokenAccessor {
 public:
  static MallocExtension::AllocationProfilingToken MakeToken(
      std::unique_ptr<AllocationProfilingTokenBase> p) {
    return MallocExtension::AllocationProfilingToken(std::move(p));
  }
};

class ProfileAccessor {
 public:
  static Profile MakeProfile(std::unique_ptr<const ProfileBase> p) {
    return Profile(std::move(p));
  }
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#if ABSL_HAVE_ATTRIBUTE_WEAK && !defined(__APPLE__) && !defined(__EMSCRIPTEN__)

extern "C" {

ABSL_ATTRIBUTE_WEAK void TCMalloc_Internal_ForceCpuCacheActivation();

ABSL_ATTRIBUTE_WEAK tcmalloc::AddressRegionFactory*
MallocExtension_Internal_GetRegionFactory();
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_SetRegionFactory(
    tcmalloc::AddressRegionFactory* factory);

ABSL_ATTRIBUTE_WEAK const tcmalloc::tcmalloc_internal::ProfileBase*
MallocExtension_Internal_SnapshotCurrent(tcmalloc::ProfileType type);

ABSL_ATTRIBUTE_WEAK tcmalloc::tcmalloc_internal::AllocationProfilingTokenBase*
MallocExtension_Internal_StartAllocationProfiling();
ABSL_ATTRIBUTE_WEAK tcmalloc::tcmalloc_internal::AllocationProfilingTokenBase*
MallocExtension_Internal_StartLifetimeProfiling();

ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_ActivateGuardedSampling();
ABSL_ATTRIBUTE_WEAK tcmalloc::MallocExtension::Ownership
MallocExtension_Internal_GetOwnership(const void* ptr);
ABSL_ATTRIBUTE_WEAK size_t MallocExtension_Internal_GetMemoryLimit(
    tcmalloc::MallocExtension::LimitKind limit_kind);
ABSL_ATTRIBUTE_WEAK bool MallocExtension_Internal_GetNumericProperty(
    const char* name_data, size_t name_size, size_t* value);
ABSL_ATTRIBUTE_WEAK bool MallocExtension_Internal_GetPerCpuCachesActive();
ABSL_ATTRIBUTE_WEAK int32_t MallocExtension_Internal_GetMaxPerCpuCacheSize();
ABSL_ATTRIBUTE_WEAK bool
MallocExtension_Internal_GetBackgroundProcessActionsEnabled();
ABSL_ATTRIBUTE_WEAK void
MallocExtension_Internal_GetBackgroundProcessSleepInterval(absl::Duration* ret);
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_GetSkipSubreleaseInterval(
    absl::Duration* ret);
ABSL_ATTRIBUTE_WEAK void
MallocExtension_Internal_GetSkipSubreleaseShortInterval(absl::Duration* ret);
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_GetSkipSubreleaseLongInterval(
    absl::Duration* ret);
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_GetProperties(
    std::map<std::string, tcmalloc::MallocExtension::Property>* ret);
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_GetExperiments(
    std::map<std::string, tcmalloc::MallocExtension::Property>* ret);
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_GetStats(std::string* ret);
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_SetMaxPerCpuCacheSize(
    int32_t value);
ABSL_ATTRIBUTE_WEAK void
MallocExtension_Internal_SetBackgroundProcessActionsEnabled(bool value);
ABSL_ATTRIBUTE_WEAK void
MallocExtension_Internal_SetBackgroundProcessSleepInterval(
    absl::Duration value);
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_SetSkipSubreleaseInterval(
    absl::Duration value);
ABSL_ATTRIBUTE_WEAK void
MallocExtension_Internal_SetSkipSubreleaseShortInterval(absl::Duration value);
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_SetSkipSubreleaseLongInterval(
    absl::Duration value);
ABSL_ATTRIBUTE_WEAK size_t MallocExtension_Internal_ReleaseCpuMemory(int cpu);
ABSL_ATTRIBUTE_WEAK size_t
MallocExtension_Internal_ReleaseMemoryToSystem(size_t bytes);
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_SetMemoryLimit(
    size_t limit, tcmalloc::MallocExtension::LimitKind limit_kind);

ABSL_ATTRIBUTE_WEAK size_t
MallocExtension_Internal_GetAllocatedSize(const void* ptr);
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_MarkThreadBusy();
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_MarkThreadIdle();

ABSL_ATTRIBUTE_WEAK int64_t MallocExtension_Internal_GetProfileSamplingRate();
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_SetProfileSamplingRate(
    int64_t);

ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_ProcessBackgroundActions();

ABSL_ATTRIBUTE_WEAK tcmalloc::MallocExtension::BytesPerSecond
MallocExtension_Internal_GetBackgroundReleaseRate();
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_SetBackgroundReleaseRate(
    tcmalloc::MallocExtension::BytesPerSecond);

ABSL_ATTRIBUTE_WEAK int64_t MallocExtension_Internal_GetGuardedSamplingRate();
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_SetGuardedSamplingRate(
    int64_t);

ABSL_ATTRIBUTE_WEAK int64_t
MallocExtension_Internal_GetMaxTotalThreadCacheBytes();
ABSL_ATTRIBUTE_WEAK void MallocExtension_Internal_SetMaxTotalThreadCacheBytes(
    int64_t value);
}

#endif

#endif  // TCMALLOC_INTERNAL_MALLOC_EXTENSION_H_
