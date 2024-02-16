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

#include "tcmalloc/internal/profile_builder.h"

#include "absl/time/time.h"
#include "tcmalloc/malloc_extension.h"

#if defined(__linux__)
#include <elf.h>
#include <link.h>
#endif  // defined(__linux__)
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "tcmalloc/internal/profile.pb.h"
#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/macros.h"
#include "absl/numeric/bits.h"
#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/residency.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

#if defined(__linux__)
// Returns the Phdr of the first segment of the given type.
const ElfW(Phdr) *
    GetFirstSegment(const dl_phdr_info* const info, const int segment_type) {
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    if (info->dlpi_phdr[i].p_type == segment_type) {
      return &info->dlpi_phdr[i];
    }
  }
  return nullptr;
}

// Return DT_SONAME for the given image.  If there is no PT_DYNAMIC or if
// PT_DYNAMIC does not contain DT_SONAME, return nullptr.
static const char* GetSoName(const dl_phdr_info* const info) {
  const ElfW(Phdr)* const pt_dynamic = GetFirstSegment(info, PT_DYNAMIC);
  if (pt_dynamic == nullptr) {
    return nullptr;
  }
  const ElfW(Dyn)* dyn =
      reinterpret_cast<ElfW(Dyn)*>(info->dlpi_addr + pt_dynamic->p_vaddr);
  const ElfW(Dyn)* dt_strtab = nullptr;
  const ElfW(Dyn)* dt_strsz = nullptr;
  const ElfW(Dyn)* dt_soname = nullptr;
  for (; dyn->d_tag != DT_NULL; ++dyn) {
    if (dyn->d_tag == DT_SONAME) {
      dt_soname = dyn;
    } else if (dyn->d_tag == DT_STRTAB) {
      dt_strtab = dyn;
    } else if (dyn->d_tag == DT_STRSZ) {
      dt_strsz = dyn;
    }
  }
  if (dt_soname == nullptr) {
    return nullptr;
  }
  CHECK_CONDITION(dt_strtab != nullptr);
  CHECK_CONDITION(dt_strsz != nullptr);
  const char* const strtab =
      reinterpret_cast<char*>(info->dlpi_addr + dt_strtab->d_un.d_val);
  CHECK_CONDITION(dt_soname->d_un.d_val < dt_strsz->d_un.d_val);
  return strtab + dt_soname->d_un.d_val;
}
#endif  // defined(__linux__)

struct SampleMergedData {
  int64_t count = 0;
  int64_t sum = 0;
  std::optional<size_t> resident_size;
  std::optional<size_t> swapped_size;
};

// The equality and hash methods of Profile::Sample only use a subset of its
// member fields.
struct SampleEqWithSubFields {
  bool operator()(const Profile::Sample& a, const Profile::Sample& b) const {
    auto fields = [](const Profile::Sample& s) {
      return std::tie(s.depth, s.requested_size, s.requested_alignment,
                      s.requested_size_returning, s.allocated_size,
                      s.access_hint, s.access_allocated);
    };
    return fields(a) == fields(b) &&
           std::equal(a.stack, a.stack + a.depth, b.stack, b.stack + b.depth);
  }
};

struct SampleHashWithSubFields {
  size_t operator()(const Profile::Sample& s) const {
    return absl::HashOf(absl::MakeConstSpan(s.stack, s.depth), s.depth,
                        s.requested_size, s.requested_alignment,
                        s.requested_size_returning, s.allocated_size,
                        s.access_hint, s.access_allocated);
  }
};

using SampleMergedMap =
    absl::flat_hash_map<const tcmalloc::Profile::Sample, SampleMergedData,
                        SampleHashWithSubFields, SampleEqWithSubFields>;

SampleMergedMap MergeProfileSamplesAndMaybeGetResidencyInfo(
    const tcmalloc::Profile& profile) {
  SampleMergedMap map;
  // Used to populate residency info in heap profile.
  std::optional<Residency> residency;

  if (profile.Type() == ProfileType::kHeap) {
    residency.emplace();
  }
  profile.Iterate([&](const tcmalloc::Profile::Sample& entry) {
    SampleMergedData& data = map[entry];
    data.count += entry.count;
    data.sum += entry.sum;
    if (residency.has_value()) {
      auto residency_info =
          residency->Get(entry.span_start_address, entry.allocated_size);
      // As long as `residency_info` provides data in some samples, the merged
      // data will have their sums.
      // NOTE: The data here is comparable to `tcmalloc::Profile::Sample::sum`,
      // not to `tcmalloc::Profile::Sample::requested_size` (it's pre-multiplied
      // by count and represents all of the resident memory).
      if (residency_info.has_value()) {
        size_t resident_size = entry.count * residency_info->bytes_resident;
        size_t swapped_size = entry.count * residency_info->bytes_swapped;
        if (!data.resident_size.has_value()) {
          data.resident_size = resident_size;
          data.swapped_size = swapped_size;
        } else {
          data.resident_size.value() += resident_size;
          data.swapped_size.value() += swapped_size;
        }
      }
    }
  });
  return map;
}

}  // namespace

#if defined(__linux__)
// Extracts the linker provided build ID from the PT_NOTE segment found in info.
//
// On failure, returns an empty string.
std::string GetBuildId(const dl_phdr_info* const info) {
  const ElfW(Phdr)* pt_note = GetFirstSegment(info, PT_NOTE);
  if (pt_note == nullptr) {
    // Failed to find note segment.
    return "";
  }
  std::string result;

  // pt_note contains entries (of type ElfW(Nhdr)) starting at
  //   info->dlpi_addr + pt_note->p_vaddr
  // with length
  //   pt_note->p_memsz
  //
  // The length of each entry is given by
  //   sizeof(ElfW(Nhdr)) + AlignTo4Bytes(nhdr->n_namesz) +
  //   AlignTo4Bytes(nhdr->n_descsz)
  const char* note =
      reinterpret_cast<char*>(info->dlpi_addr + pt_note->p_vaddr);
  const char* const last = note + pt_note->p_memsz;
  while (note < last) {
    const ElfW(Nhdr)* const nhdr = reinterpret_cast<const ElfW(Nhdr)*>(note);
    if (note + sizeof(*nhdr) > last) {
      // Corrupt PT_NOTE
      break;
    }

    ElfW(Word) name_size = nhdr->n_namesz;
    ElfW(Word) desc_size = nhdr->n_descsz;
    if (name_size >= static_cast<ElfW(Word)>(-3) ||
        desc_size >= static_cast<ElfW(Word)>(-3)) {
      // These would wrap around when aligned.  The PT_NOTE is corrupt.
      break;
    }

    // Beware of overflows / wrap-around.
    if (name_size >= pt_note->p_memsz || desc_size >= pt_note->p_memsz ||
        note + sizeof(*nhdr) + name_size + desc_size > last) {
      // Corrupt PT_NOTE
      break;
    }

    if (nhdr->n_type == NT_GNU_BUILD_ID) {
      const char* const note_name = note + sizeof(*nhdr);
      // n_namesz is the length of note_name.
      if (name_size == 4 && memcmp(note_name, "GNU\0", 4) == 0) {
        if (!result.empty()) {
          // Repeated build-ids.  Ignore them.
          return "";
        }
        const char* note_data =
            reinterpret_cast<const char*>(nhdr) + sizeof(*nhdr) + name_size;
        result =
            absl::BytesToHexString(absl::string_view(note_data, desc_size));
      }
    }

    // Align name_size, desc_size.
    name_size = (name_size + 3) & ~3;
    desc_size = (desc_size + 3) & ~3;

    note += name_size + desc_size + sizeof(*nhdr);
  }
  return result;
}
#endif  // defined(__linux__)

ABSL_CONST_INIT const absl::string_view kProfileDropFrames =
    // POSIX entry points.
    "calloc|"
    "cfree|"
    "malloc|"
    "free|"
    "memalign|"
    "do_memalign|"
    "(__)?posix_memalign|"
    "pvalloc|"
    "valloc|"
    "realloc|"

    // TCMalloc.
    "tcmalloc::.*|"
    "TCMallocInternalCalloc|"
    "TCMallocInternalCfree|"
    "TCMallocInternalMalloc|"
    "TCMallocInternalFree|"
    "TCMallocInternalMemalign|"
    "TCMallocInternalAlignedAlloc|"
    "TCMallocInternalPosixMemalign|"
    "TCMallocInternalPvalloc|"
    "TCMallocInternalValloc|"
    "TCMallocInternalRealloc|"
    "TCMallocInternalNew(Array)?(Aligned)?(Nothrow)?|"
    "TCMallocInternalDelete(Array)?(Sized)?(Aligned)?(Nothrow)?|"
    "TCMallocInternalSdallocx|"
    "(tcmalloc_)?size_returning_operator_new(_hot_cold)?(_nothrow)?|"

    // Lifetime (deallocation) profiler routines.
    ".*deallocationz::DeallocationProfiler.*|"

    // libstdc++ memory allocation routines
    "__gnu_cxx::new_allocator::allocate|"
    "__malloc_alloc_template::allocate|"
    "_M_allocate|"

    // libc++ memory allocation routines
    "std::__u::__libcpp_allocate|"
    "std::__u::allocator::allocate|"
    "std::__u::allocator_traits::allocate|"

    // Other misc. memory allocation routines
    "(::)?do_malloc_pages|"
    "(::)?do_realloc|"
    "__builtin_(vec_)?delete|"
    "__builtin_(vec_)?new|"
    "__libc_calloc|"
    "__libc_malloc|"
    "__libc_memalign|"
    "__libc_realloc|"
    "(::)?slow_alloc|"
    "fast_alloc|"
    "(::)?AllocSmall|"
    "operator new(\\[\\])?";

ProfileBuilder::ProfileBuilder()
    : profile_(std::make_unique<perftools::profiles::Profile>()) {
  // string_table[0] must be ""
  profile_->add_string_table("");
}

int ProfileBuilder::InternString(absl::string_view sv) {
  if (sv.empty()) {
    return 0;
  }

  const int index = profile_->string_table_size();
  const auto inserted = strings_.emplace(sv, index);
  if (!inserted.second) {
    // Failed to insert -- use existing id.
    return inserted.first->second;
  }
  profile_->add_string_table(inserted.first->first);
  return index;
}

int ProfileBuilder::InternLocation(const void* ptr) {
  uintptr_t address = absl::bit_cast<uintptr_t>(ptr);

  // Avoid assigning location ID 0 by incrementing by 1.
  const int index = profile_->location_size() + 1;
  const auto inserted = locations_.emplace(address, index);
  if (!inserted.second) {
    // Failed to insert -- use existing id.
    return inserted.first->second;
  }
  perftools::profiles::Location& location = *profile_->add_location();
  ASSERT(inserted.first->second == index);
  location.set_id(index);
  location.set_address(address);

  if (mappings_.empty()) {
    return index;
  }

  // Find the mapping ID.
  auto it = mappings_.upper_bound(address);
  if (it != mappings_.begin()) {
    --it;
  }

  // If *it contains address, add mapping to location.
  const int mapping_index = it->second;
  const perftools::profiles::Mapping& mapping =
      profile_->mapping(mapping_index);
  const int mapping_id = mapping.id();
  ASSERT(it->first == mapping.memory_start());

  if (it->first <= address && address < mapping.memory_limit()) {
    location.set_mapping_id(mapping_id);
  }

  return index;
}

void ProfileBuilder::InternCallstack(absl::Span<const void* const> stack,
                                     perftools::profiles::Sample& sample) {
  // Profile addresses are raw stack unwind addresses, so they should be
  // adjusted by -1 to land inside the call instruction (although potentially
  // misaligned).
  for (const void* frame : stack) {
    int id = InternLocation(
        absl::bit_cast<const void*>(absl::bit_cast<uintptr_t>(frame) - 1));
    sample.add_location_id(id);
  }
  ASSERT(sample.location_id().size() == stack.size());
}

void ProfileBuilder::AddCurrentMappings() {
#if defined(__linux__)
  auto dl_iterate_callback = +[](dl_phdr_info* info, size_t size, void* data) {
    // Skip dummy entry introduced since glibc 2.18.
    if (info->dlpi_phdr == nullptr && info->dlpi_phnum == 0) {
      return 0;
    }

    ProfileBuilder& builder = *static_cast<ProfileBuilder*>(data);
    const bool is_main_executable = builder.profile_->mapping_size() == 0;

    // Evaluate all the loadable segments.
    for (int i = 0; i < info->dlpi_phnum; ++i) {
      if (info->dlpi_phdr[i].p_type != PT_LOAD) {
        continue;
      }
      const ElfW(Phdr)* pt_load = &info->dlpi_phdr[i];

      CHECK_CONDITION(pt_load != nullptr);

      // Extract data.
      const size_t memory_start = info->dlpi_addr + pt_load->p_vaddr;
      const size_t memory_limit = memory_start + pt_load->p_memsz;
      const size_t file_offset = pt_load->p_offset;

      // Storage for path to executable as dlpi_name isn't populated for the
      // main executable.  +1 to allow for the null terminator that readlink
      // does not add.
      char self_filename[PATH_MAX + 1];
      const char* filename = info->dlpi_name;
      if (filename == nullptr || filename[0] == '\0') {
        // This is either the main executable or the VDSO.  The main executable
        // is always the first entry processed by callbacks.
        if (is_main_executable) {
          // This is the main executable.
          ssize_t ret = readlink("/proc/self/exe", self_filename,
                                 sizeof(self_filename) - 1);
          if (ret >= 0 && ret < sizeof(self_filename)) {
            self_filename[ret] = '\0';
            filename = self_filename;
          }
        } else {
          // This is the VDSO.
          filename = GetSoName(info);
        }
      }

      char resolved_path[PATH_MAX];
      absl::string_view resolved_filename;
      if (realpath(filename, resolved_path)) {
        resolved_filename = resolved_path;
      } else {
        resolved_filename = filename;
      }

      const std::string build_id = GetBuildId(info);

      // Add to profile.
      builder.AddMapping(memory_start, memory_limit, file_offset,
                         resolved_filename, build_id);
    }
    // Keep going.
    return 0;
  };

  dl_iterate_phdr(dl_iterate_callback, this);
#endif  // defined(__linux__)
}

void ProfileBuilder::AddMapping(uintptr_t memory_start, uintptr_t memory_limit,
                                uintptr_t file_offset,
                                absl::string_view filename,
                                absl::string_view build_id) {
  perftools::profiles::Mapping& mapping = *profile_->add_mapping();
  mapping.set_id(profile_->mapping_size());
  mapping.set_memory_start(memory_start);
  mapping.set_memory_limit(memory_limit);
  mapping.set_file_offset(file_offset);
  mapping.set_filename(InternString(filename));
  mapping.set_build_id(InternString(build_id));

  mappings_.emplace(memory_start, mapping.id() - 1);
}

static void MakeLifetimeProfileProto(const tcmalloc::Profile& profile,
                                     ProfileBuilder* builder) {
  CHECK_CONDITION(builder != nullptr);
  perftools::profiles::Profile& converted = builder->profile();
  perftools::profiles::ValueType* period_type = converted.mutable_period_type();

  period_type->set_type(builder->InternString("space"));
  period_type->set_unit(builder->InternString("bytes"));

  for (const auto& [type, unit] : {std::pair{"allocated_objects", "count"},
                                   {"allocated_space", "bytes"},
                                   {"deallocated_objects", "count"},
                                   {"deallocated_space", "bytes"},
                                   {"censored_allocated_objects", "count"},
                                   {"censored_allocated_space", "bytes"}}) {
    perftools::profiles::ValueType* sample_type = converted.add_sample_type();
    sample_type->set_type(builder->InternString(type));
    sample_type->set_unit(builder->InternString(unit));
  }

  converted.set_default_sample_type(builder->InternString("deallocated_space"));
  converted.set_duration_nanos(absl::ToInt64Nanoseconds(profile.Duration()));
  converted.set_drop_frames(builder->InternString(kProfileDropFrames));

  // Common intern string ids which are going to be used for each sample.
  const int count_id = builder->InternString("count");
  const int bytes_id = builder->InternString("bytes");
  const int request_id = builder->InternString("request");
  const int alignment_id = builder->InternString("alignment");
  const int nanoseconds_id = builder->InternString("nanoseconds");
  const int avg_lifetime_id = builder->InternString("avg_lifetime");
  const int stddev_lifetime_id = builder->InternString("stddev_lifetime");
  const int min_lifetime_id = builder->InternString("min_lifetime");
  const int max_lifetime_id = builder->InternString("max_lifetime");
  const int active_cpu_id = builder->InternString("active CPU");
  const int same_id = builder->InternString("same");
  const int different_id = builder->InternString("different");
  const int active_thread_id = builder->InternString("active thread");
  const int callstack_pair_id = builder->InternString("callstack-pair-id");
  const int none_id = builder->InternString("none");

  profile.Iterate([&](const tcmalloc::Profile::Sample& entry) {
    perftools::profiles::Sample& sample = *converted.add_sample();

    CHECK_CONDITION(entry.depth <= ABSL_ARRAYSIZE(entry.stack));
    builder->InternCallstack(absl::MakeSpan(entry.stack, entry.depth), sample);

    auto add_label = [&](int key, int unit, size_t value) {
      perftools::profiles::Label& label = *sample.add_label();
      label.set_key(key);
      label.set_num(value);
      label.set_num_unit(unit);
    };

    auto add_positive_label = [&](int key, int unit, size_t value) {
      if (value <= 0) return;
      add_label(key, unit, value);
    };

    auto add_optional_string_label =
        [&](int key, const std::optional<bool>& optional_result, int result1,
            int result2) {
          perftools::profiles::Label& label = *sample.add_label();
          label.set_key(key);

          if (!optional_result.has_value()) {
            label.set_str(none_id);
          } else if (optional_result.value()) {
            label.set_str(result1);
          } else {
            label.set_str(result2);
          }
        };

    // The following three fields are common across profiles.
    add_positive_label(bytes_id, bytes_id, entry.allocated_size);
    add_positive_label(request_id, bytes_id, entry.requested_size);
    add_positive_label(alignment_id, bytes_id, entry.requested_alignment);

    // The following fields are specific to lifetime (deallocation) profiler.
    add_positive_label(callstack_pair_id, count_id, entry.profile_id);
    add_positive_label(avg_lifetime_id, nanoseconds_id,
                       absl::ToInt64Nanoseconds(entry.avg_lifetime));
    add_positive_label(stddev_lifetime_id, nanoseconds_id,
                       absl::ToInt64Nanoseconds(entry.stddev_lifetime));
    add_positive_label(min_lifetime_id, nanoseconds_id,
                       absl::ToInt64Nanoseconds(entry.min_lifetime));
    add_positive_label(max_lifetime_id, nanoseconds_id,
                       absl::ToInt64Nanoseconds(entry.max_lifetime));

    add_optional_string_label(active_cpu_id,
                              entry.allocator_deallocator_cpu_matched, same_id,
                              different_id);
    add_optional_string_label(active_thread_id,
                              entry.allocator_deallocator_thread_matched,
                              same_id, different_id);

    int64_t count = abs(entry.count);
    int64_t weight = entry.sum;

    // Handle censored allocations first since we distinguish
    // the samples based on the is_censored flag.
    if (entry.is_censored) {
      sample.add_value(0);
      sample.add_value(0);
      sample.add_value(0);
      sample.add_value(0);
      sample.add_value(count);
      sample.add_value(weight);
    } else if (entry.count > 0) {  // for allocation, e.count is positive
      sample.add_value(count);
      sample.add_value(weight);
      sample.add_value(0);
      sample.add_value(0);
      sample.add_value(0);
      sample.add_value(0);
    } else {  // for deallocation, e.count is negative
      sample.add_value(0);
      sample.add_value(0);
      sample.add_value(count);
      sample.add_value(weight);
      sample.add_value(0);
      sample.add_value(0);
    }
  });
}

std::unique_ptr<perftools::profiles::Profile> ProfileBuilder::Finalize() && {
  return std::move(profile_);
}

absl::StatusOr<std::unique_ptr<perftools::profiles::Profile>> MakeProfileProto(
    const ::tcmalloc::Profile& profile) {
  ProfileBuilder builder;
  builder.AddCurrentMappings();

  if (profile.Type() == ProfileType::kLifetimes) {
    MakeLifetimeProfileProto(profile, &builder);
    return std::move(builder).Finalize();
  }

  const int alignment_id = builder.InternString("alignment");
  const int bytes_id = builder.InternString("bytes");
  const int count_id = builder.InternString("count");
  const int objects_id = builder.InternString("objects");
  const int request_id = builder.InternString("request");
  const int size_returning_id = builder.InternString("size_returning");
  const int space_id = builder.InternString("space");
  const int resident_space_id = builder.InternString("resident_space");
  const int swapped_space_id = builder.InternString("swapped_space");
  const int access_hint_id = builder.InternString("access_hint");
  const int access_allocated_id = builder.InternString("access_allocated");
  const int cold_id = builder.InternString("cold");
  const int hot_id = builder.InternString("hot");

  // NOTE: Do not rely on these string constants. They will be removed!
  // TODO(b/259585789): Remove all of these tags when sample type rollout
  // and collection hits close to 100%; certainly by Q3 2023, but could consider
  // earlier.
  const int sampled_resident_id =
      builder.InternString("sampled_resident_bytes");
  const int swapped_id = builder.InternString("swapped_bytes");

  perftools::profiles::Profile& converted = builder.profile();

  perftools::profiles::ValueType& period_type =
      *converted.mutable_period_type();
  period_type.set_type(space_id);
  period_type.set_unit(bytes_id);
  converted.set_drop_frames(builder.InternString(kProfileDropFrames));

  converted.set_duration_nanos(absl::ToInt64Nanoseconds(profile.Duration()));

  {
    perftools::profiles::ValueType& sample_type = *converted.add_sample_type();
    sample_type.set_type(objects_id);
    sample_type.set_unit(count_id);
  }

  {
    perftools::profiles::ValueType& sample_type = *converted.add_sample_type();
    sample_type.set_type(space_id);
    sample_type.set_unit(bytes_id);
  }

  const bool exporting_residency =
      (profile.Type() == tcmalloc::ProfileType::kHeap);
  if (exporting_residency) {
    perftools::profiles::ValueType* sample_type = converted.add_sample_type();
    sample_type->set_type(resident_space_id);
    sample_type->set_unit(bytes_id);

    sample_type = converted.add_sample_type();
    sample_type->set_type(swapped_space_id);
    sample_type->set_unit(bytes_id);
  }

  int default_sample_type_id;
  switch (profile.Type()) {
    case tcmalloc::ProfileType::kFragmentation:
    case tcmalloc::ProfileType::kHeap:
    case tcmalloc::ProfileType::kPeakHeap:
      default_sample_type_id = space_id;
      break;
    case tcmalloc::ProfileType::kAllocations:
      default_sample_type_id = objects_id;
      break;
    default:
#if defined(ABSL_HAVE_ADDRESS_SANITIZER) || \
    defined(ABSL_HAVE_LEAK_SANITIZER) ||    \
    defined(ABSL_HAVE_MEMORY_SANITIZER) || defined(ABSL_HAVE_THREAD_SANITIZER)
      return absl::UnimplementedError(
          "Program was built with sanitizers enabled, which do not support "
          "heap profiling");
#endif
      return absl::InvalidArgumentError("Unexpected profile format");
  }

  converted.set_default_sample_type(default_sample_type_id);

  SampleMergedMap samples =
      MergeProfileSamplesAndMaybeGetResidencyInfo(profile);
  for (const auto& [entry, data] : samples) {
    perftools::profiles::Profile& profile = builder.profile();
    perftools::profiles::Sample& sample = *profile.add_sample();

    CHECK_CONDITION(entry.depth <= ABSL_ARRAYSIZE(entry.stack));
    builder.InternCallstack(absl::MakeSpan(entry.stack, entry.depth), sample);

    sample.add_value(data.count);
    sample.add_value(data.sum);
    if (exporting_residency) {
      sample.add_value(data.resident_size.value_or(0));
      sample.add_value(data.swapped_size.value_or(0));
    }

    // add fields that are common to all memory profiles
    auto add_label = [&](int key, int unit, size_t value) {
      perftools::profiles::Label& label = *sample.add_label();
      label.set_key(key);
      label.set_num(value);
      label.set_num_unit(unit);
    };

    auto add_positive_label = [&](int key, int unit, size_t value) {
      if (value <= 0) return;
      add_label(key, unit, value);
    };

    add_positive_label(bytes_id, bytes_id, entry.allocated_size);
    add_positive_label(request_id, bytes_id, entry.requested_size);
    add_positive_label(alignment_id, bytes_id, entry.requested_alignment);
    add_positive_label(size_returning_id, 0, entry.requested_size_returning);
    // TODO(b/259585789): Remove all of these when sample type rollout is
    // complete.
    if (data.resident_size.has_value()) {
      add_label(sampled_resident_id, bytes_id, data.resident_size.value());
      add_label(swapped_id, bytes_id, data.swapped_size.value());
    }

    auto add_access_label = [&](int key,
                                tcmalloc::Profile::Sample::Access access) {
      switch (access) {
        case tcmalloc::Profile::Sample::Access::Hot: {
          perftools::profiles::Label& access_label = *sample.add_label();
          access_label.set_key(key);
          access_label.set_str(hot_id);
          break;
        }
        case tcmalloc::Profile::Sample::Access::Cold: {
          perftools::profiles::Label& access_label = *sample.add_label();
          access_label.set_key(key);
          access_label.set_str(cold_id);
          break;
        }
        default:
          break;
      }
    };

    add_label(access_hint_id, access_hint_id,
              static_cast<uint8_t>(entry.access_hint));
    add_access_label(access_allocated_id, entry.access_allocated);

    const int guarded_status_id = builder.InternString("guarded_status");
    const int larger_than_one_page_id =
        builder.InternString("LargerThanOnePage");
    const int disabled_id = builder.InternString("Disabled");
    const int rate_limited_id = builder.InternString("RateLimited");
    const int too_small_id = builder.InternString("TooSmall");
    const int no_available_slots_id = builder.InternString("NoAvailableSlots");
    const int m_protect_failed_id = builder.InternString("MProtectFailed");
    const int filtered_id = builder.InternString("Filtered");
    const int unknown_id = builder.InternString("Unknown");
    const int not_attempted_id = builder.InternString("NotAttempted");
    const int requested_id = builder.InternString("Requested");
    const int required_id = builder.InternString("Required");
    const int guarded_id = builder.InternString("Guarded");

    perftools::profiles::Label& guarded_status_label = *sample.add_label();
    guarded_status_label.set_key(guarded_status_id);
    switch (entry.guarded_status) {
      case Profile::Sample::GuardedStatus::LargerThanOnePage:
        guarded_status_label.set_str(larger_than_one_page_id);
        break;
      case Profile::Sample::GuardedStatus::Disabled:
        guarded_status_label.set_str(disabled_id);
        break;
      case Profile::Sample::GuardedStatus::RateLimited:
        guarded_status_label.set_str(rate_limited_id);
        break;
      case Profile::Sample::GuardedStatus::TooSmall:
        guarded_status_label.set_str(too_small_id);
        break;
      case Profile::Sample::GuardedStatus::NoAvailableSlots:
        guarded_status_label.set_str(no_available_slots_id);
        break;
      case Profile::Sample::GuardedStatus::MProtectFailed:
        guarded_status_label.set_str(m_protect_failed_id);
        break;
      case Profile::Sample::GuardedStatus::Filtered:
        guarded_status_label.set_str(filtered_id);
        break;
      case Profile::Sample::GuardedStatus::Unknown:
        guarded_status_label.set_str(unknown_id);
        break;
      case Profile::Sample::GuardedStatus::NotAttempted:
        guarded_status_label.set_str(not_attempted_id);
        break;
      case Profile::Sample::GuardedStatus::Requested:
        guarded_status_label.set_str(requested_id);
        break;
      case Profile::Sample::GuardedStatus::Required:
        guarded_status_label.set_str(required_id);
        break;
      case Profile::Sample::GuardedStatus::Guarded:
        guarded_status_label.set_str(guarded_id);
        break;
    }
  }

  return std::move(builder).Finalize();
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
