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

#ifndef TCMALLOC_PARAMETERS_H_
#define TCMALLOC_PARAMETERS_H_

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/parameter_accessors.h"
#include "tcmalloc/malloc_extension.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class Parameters {
 public:
  static MallocExtension::BytesPerSecond background_release_rate() {
    return background_release_rate_.load(std::memory_order_relaxed);
  }

  static void set_background_release_rate(
      MallocExtension::BytesPerSecond value) {
    TCMalloc_Internal_SetBackgroundReleaseRate(static_cast<size_t>(value));
  }

  static uint64_t heap_size_hard_limit();
  static void set_heap_size_hard_limit(uint64_t value);

  static bool hpaa_subrelease();
  static void set_hpaa_subrelease(bool value);

  static bool use_all_buckets_for_few_object_spans_in_cfl();

  static int64_t guarded_sampling_rate() {
    return guarded_sampling_rate_.load(std::memory_order_relaxed);
  }

  static void set_guarded_sampling_rate(int64_t value) {
    TCMalloc_Internal_SetGuardedSamplingRate(value);
  }

  static int32_t max_per_cpu_cache_size();

  static void set_max_per_cpu_cache_size(int32_t value) {
    TCMalloc_Internal_SetMaxPerCpuCacheSize(value);
  }

  static int64_t max_total_thread_cache_bytes() {
    return max_total_thread_cache_bytes_.load(std::memory_order_relaxed);
  }

  static bool madvise_free() {
    return madvise_free_.load(std::memory_order_relaxed);
  }

  static void set_madvise_free(bool value) {
    TCMalloc_Internal_SetMadviseFree(value);
  }

  static MadvisePreference madvise() {
    return madvise_.load(std::memory_order_relaxed);
  }

  static void set_madvise_free(MadvisePreference value) {
    TCMalloc_Internal_SetMadvise(value);
  }

  static tcmalloc::hot_cold_t min_hot_access_hint() {
    return min_hot_access_hint_.load(std::memory_order_relaxed);
  }

  static void set_min_hot_access_hint(tcmalloc::hot_cold_t value) {
    TCMalloc_Internal_SetMinHotAccessHint(static_cast<uint8_t>(value));
  }

  static void set_max_total_thread_cache_bytes(int64_t value) {
    TCMalloc_Internal_SetMaxTotalThreadCacheBytes(value);
  }

  static double peak_sampling_heap_growth_fraction() {
    return peak_sampling_heap_growth_fraction_.load(std::memory_order_relaxed);
  }

  static void set_peak_sampling_heap_growth_fraction(double value) {
    TCMalloc_Internal_SetPeakSamplingHeapGrowthFraction(value);
  }

  static bool release_partial_alloc_pages() {
    return release_partial_alloc_pages_.load(std::memory_order_relaxed);
  }

  static bool huge_region_demand_based_release() {
    return huge_region_demand_based_release_.load(std::memory_order_relaxed);
  }

  static bool release_pages_from_huge_region() {
    return release_pages_from_huge_region_.load(std::memory_order_relaxed);
  }

  static bool per_cpu_caches() {
    return per_cpu_caches_enabled_.load(std::memory_order_relaxed);
  }

  static void set_per_cpu_caches(bool value) {
#if !defined(TCMALLOC_DEPRECATED_PERTHREAD)
    if (!value) {
      TC_LOG(
          "Using per-thread caches requires linking against "
          ":tcmalloc_deprecated_perthread.");
      return;
    }
#endif  // !TCMALLOC_DEPRECATED_PERTHREAD

    TCMalloc_Internal_SetPerCpuCachesEnabled(value);
  }

  static uint32_t max_span_cache_size() {
    return max_span_cache_size_.load(std::memory_order_relaxed);
  }

  static void set_max_span_cache_size(uint32_t v) {
    max_span_cache_size_.store(v, std::memory_order_relaxed);
  }

  static int64_t profile_sampling_rate() {
    return profile_sampling_rate_.load(std::memory_order_relaxed);
  }

  static void set_profile_sampling_rate(int64_t value) {
    TCMalloc_Internal_SetProfileSamplingRate(value);
  }

  static void set_filler_skip_subrelease_interval(absl::Duration value) {
    TCMalloc_Internal_SetHugePageFillerSkipSubreleaseInterval(value);
  }

  static bool background_process_actions_enabled();
  static absl::Duration background_process_sleep_interval();

  static absl::Duration filler_skip_subrelease_interval();

  static void set_filler_skip_subrelease_short_interval(absl::Duration value) {
    TCMalloc_Internal_SetHugePageFillerSkipSubreleaseShortInterval(value);
  }

  static absl::Duration filler_skip_subrelease_short_interval();

  static void set_filler_skip_subrelease_long_interval(absl::Duration value) {
    TCMalloc_Internal_SetHugePageFillerSkipSubreleaseLongInterval(value);
  }

  static absl::Duration filler_skip_subrelease_long_interval();

  static bool per_cpu_caches_dynamic_slab_enabled() {
    return per_cpu_caches_dynamic_slab_.load(std::memory_order_relaxed);
  }
  static void set_per_cpu_caches_dynamic_slab_enabled(bool value) {
    TCMalloc_Internal_SetPerCpuCachesDynamicSlabEnabled(value);
  }

  static double per_cpu_caches_dynamic_slab_grow_threshold() {
    return per_cpu_caches_dynamic_slab_grow_threshold_.load(
        std::memory_order_relaxed);
  }
  static void set_per_cpu_caches_dynamic_slab_grow_threshold(double value) {
    TCMalloc_Internal_SetPerCpuCachesDynamicSlabGrowThreshold(value);
  }

  static double per_cpu_caches_dynamic_slab_shrink_threshold() {
    return per_cpu_caches_dynamic_slab_shrink_threshold_.load(
        std::memory_order_relaxed);
  }
  static void set_per_cpu_caches_dynamic_slab_shrink_threshold(double value) {
    TCMalloc_Internal_SetPerCpuCachesDynamicSlabShrinkThreshold(value);
  }

  static bool separate_allocs_for_few_and_many_objects_spans();
  static size_t chunks_per_alloc();

 private:
  friend void ::TCMalloc_Internal_SetBackgroundReleaseRate(size_t v);
  friend void ::TCMalloc_Internal_SetGuardedSamplingRate(int64_t v);
  friend void ::TCMalloc_Internal_SetHPAASubrelease(bool v);
  friend void ::TCMalloc_Internal_SetReleasePartialAllocPagesEnabled(bool v);
  friend void ::TCMalloc_Internal_SetHugeRegionDemandBasedRelease(bool v);
  friend void ::TCMalloc_Internal_SetReleasePagesFromHugeRegionEnabled(bool v);
  friend void ::TCMalloc_Internal_SetMaxPerCpuCacheSize(int32_t v);
  friend void ::TCMalloc_Internal_SetMaxTotalThreadCacheBytes(int64_t v);
  friend void ::TCMalloc_Internal_SetPeakSamplingHeapGrowthFraction(double v);
  friend void ::TCMalloc_Internal_SetPerCpuCachesEnabled(bool v);
  friend void ::TCMalloc_Internal_SetProfileSamplingRate(int64_t v);

  friend void ::TCMalloc_Internal_SetHugePageFillerSkipSubreleaseInterval(
      absl::Duration v);
  friend void ::TCMalloc_Internal_SetHugePageFillerSkipSubreleaseShortInterval(
      absl::Duration v);
  friend void ::TCMalloc_Internal_SetHugePageFillerSkipSubreleaseLongInterval(
      absl::Duration v);
  friend void ::TCMalloc_Internal_SetPerCpuCachesDynamicSlabEnabled(bool v);
  friend void ::TCMalloc_Internal_SetPerCpuCachesDynamicSlabGrowThreshold(
      double v);
  friend void ::TCMalloc_Internal_SetPerCpuCachesDynamicSlabShrinkThreshold(
      double v);

  friend void TCMalloc_Internal_SetLifetimeAllocatorOptions(
      absl::string_view s);
  friend void ::TCMalloc_Internal_SetMadvise(
      tcmalloc::tcmalloc_internal::MadvisePreference v);
  friend void ::TCMalloc_Internal_SetMadviseFree(bool v);
  friend void ::TCMalloc_Internal_SetMinHotAccessHint(uint8_t v);

  static std::atomic<MallocExtension::BytesPerSecond> background_release_rate_;
  static std::atomic<int64_t> guarded_sampling_rate_;
  static std::atomic<uint32_t> max_span_cache_size_;
  static std::atomic<int32_t> max_per_cpu_cache_size_;
  static std::atomic<int64_t> max_total_thread_cache_bytes_;
  static std::atomic<double> peak_sampling_heap_growth_fraction_;
  static std::atomic<bool> per_cpu_caches_enabled_;
  static std::atomic<bool> release_partial_alloc_pages_;
  static std::atomic<bool> huge_region_demand_based_release_;
  static std::atomic<bool> release_pages_from_huge_region_;
  static std::atomic<int64_t> profile_sampling_rate_;
  static std::atomic<bool> per_cpu_caches_dynamic_slab_;
  static std::atomic<MadvisePreference> madvise_;
  static std::atomic<bool> madvise_free_;
  static std::atomic<tcmalloc::hot_cold_t> min_hot_access_hint_;
  static std::atomic<double> per_cpu_caches_dynamic_slab_grow_threshold_;
  static std::atomic<double> per_cpu_caches_dynamic_slab_shrink_threshold_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_PARAMETERS_H_
