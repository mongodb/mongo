// Copyright 2021 The TCMalloc Authors
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

#ifndef TCMALLOC_INTERNAL_PROFILE_BUILDER_H_
#define TCMALLOC_INTERNAL_PROFILE_BUILDER_H_

#if defined(__linux__)
#include <elf.h>
#include <link.h>
#endif  // defined(__linux__)

#include <cstdint>
#include <memory>
#include <string>

#include "tcmalloc/internal/profile.pb.h"
#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {
namespace tcmalloc_internal {

#if defined(__linux__)
std::string GetBuildId(const dl_phdr_info* const info);
#endif  // defined(__linux__)

// ProfileBuilder manages building up a profile.proto instance and populating
// common parts using the string/pointer table conventions expected by pprof.
class ProfileBuilder {
 public:
  ProfileBuilder();

  perftools::profiles::Profile& profile() { return *profile_; }

  // Adds the current process mappings to the profile.
  void AddCurrentMappings();

  // Adds a single mapping to the profile and to lookup cache and returns the
  // resulting ID.
  int AddMapping(uintptr_t memory_start, uintptr_t memory_limit,
                 uintptr_t file_offset, absl::string_view filename,
                 absl::string_view build_id);

  // Interns sv in the profile's string table and returns the resulting ID.
  int InternString(absl::string_view sv);
  // Interns a location in the profile's location table and returns the
  // resulting ID.
  int InternLocation(const void* ptr);

  // Interns a callstack and adds the IDs to the provided sample.
  void InternCallstack(absl::Span<const void* const> stack,
                       perftools::profiles::Sample& sample);

  std::unique_ptr<perftools::profiles::Profile> Finalize() &&;

 private:
  std::unique_ptr<perftools::profiles::Profile> profile_;
  // mappings_ stores the start address of each mapping in profile_->mapping()
  // to its index.
  absl::btree_map<uintptr_t, int> mappings_;
  absl::flat_hash_map<std::string, int> strings_;
  absl::flat_hash_map<uintptr_t, int> locations_;
};

extern const absl::string_view kProfileDropFrames;

absl::StatusOr<std::unique_ptr<perftools::profiles::Profile>> MakeProfileProto(
    const ::tcmalloc::Profile& profile);

class PageFlags;
class Residency;

// Exposed to facilitate testing.
absl::StatusOr<std::unique_ptr<perftools::profiles::Profile>> MakeProfileProto(
    const ::tcmalloc::Profile& profile, PageFlags* pageflags,
    Residency* residency);

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_INTERNAL_PROFILE_BUILDER_H_
