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

#include <stddef.h>

#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "absl/types/optional.h"
#include "tcmalloc/common.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/parameters.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

using tcmalloc_internal::Parameters;
using ::testing::AnyOf;
using ::testing::ContainsRegex;
using ::testing::HasSubstr;

class GetStatsTest : public ::testing::Test {};

TEST_F(GetStatsTest, Pbtxt) {
  // Trigger a sampled allocation.
  { ScopedAlwaysSample s; }

  const std::string buf = GetStatsInPbTxt();

  absl::optional<size_t> fragmentation = MallocExtension::GetNumericProperty(
      "tcmalloc.sampled_internal_fragmentation");
  ASSERT_THAT(fragmentation, testing::Ne(absl::nullopt));

  // Expect `buf` to be in pbtxt format.
  EXPECT_THAT(buf, ContainsRegex(R"(in_use_by_app: [0-9]+)"));
  EXPECT_THAT(buf, ContainsRegex(R"(page_heap_freelist: [0-9]+)"));
  EXPECT_THAT(buf, ContainsRegex(R"(tcmalloc_huge_page_size: [0-9]+)"));
#if defined(GTEST_USES_PCRE)
  EXPECT_THAT(buf,
              ContainsRegex(absl::StrCat(
                  R"(freelist\s{\s*sizeclass:\s\d+\s*bytes:\s\d+\s*)",
                  R"(num_spans_requested:\s\d+\s*num_spans_returned:\s\d+\s*)",
                  R"(obj_capacity:\s\d+\s*)")));
#endif  // defined(GTEST_USES_PCRE)
  EXPECT_THAT(buf, AnyOf(HasSubstr(R"(page_heap {)"),
                         HasSubstr(R"(huge_page_aware {)")));
  EXPECT_THAT(buf, HasSubstr(R"(gwp_asan {)"));

  EXPECT_THAT(buf, ContainsRegex(R"(mmap_sys_allocator: [0-9]*)"));
  EXPECT_THAT(buf, HasSubstr("memory_release_failures: 0"));

  if (MallocExtension::PerCpuCachesActive()) {
    EXPECT_THAT(buf, ContainsRegex(R"(per_cpu_cache_freelist: [1-9][0-9]*)"));
    EXPECT_THAT(buf, ContainsRegex(R"(percpu_slab_size: [1-9][0-9]*)"));
    EXPECT_THAT(buf, ContainsRegex(R"(percpu_slab_residence: [1-9][0-9]*)"));
  } else {
    EXPECT_THAT(buf, HasSubstr("per_cpu_cache_freelist: 0"));
    EXPECT_THAT(buf, HasSubstr("percpu_slab_size: 0"));
    EXPECT_THAT(buf, HasSubstr("percpu_slab_residence: 0"));
  }
  EXPECT_THAT(buf, ContainsRegex("(cpus_allowed: [1-9][0-9]*)"));
  EXPECT_THAT(buf, HasSubstr("transfer_cache_implementation: LIFO"));

  EXPECT_THAT(buf, HasSubstr("desired_usage_limit_bytes: -1"));
  EXPECT_THAT(buf,
              HasSubstr(absl::StrCat("profile_sampling_rate: ",
                                     Parameters::profile_sampling_rate())));
  EXPECT_THAT(buf, HasSubstr("limit_hits: 0"));
  EXPECT_THAT(buf,
              HasSubstr("tcmalloc_skip_subrelease_interval_ns: 60000000000"));
  EXPECT_THAT(buf, HasSubstr("tcmalloc_skip_subrelease_short_interval_ns: 0"));
  EXPECT_THAT(buf, HasSubstr("tcmalloc_skip_subrelease_long_interval_ns: 0"));
}

TEST_F(GetStatsTest, Parameters) {
#ifdef __x86_64__
  // HPAA is not enabled by default for non-x86 platforms, so we do not print
  // parameters related to it (like subrelease) in these situations.
  Parameters::set_hpaa_subrelease(false);
#endif
  Parameters::set_guarded_sampling_rate(-1);
  Parameters::set_per_cpu_caches(false);
  Parameters::set_max_per_cpu_cache_size(-1);
  Parameters::set_max_total_thread_cache_bytes(-1);
  Parameters::set_filler_skip_subrelease_interval(absl::Seconds(1));
  Parameters::set_filler_skip_subrelease_short_interval(absl::Seconds(2));
  Parameters::set_filler_skip_subrelease_long_interval(absl::Seconds(3));
  {
    const std::string buf = MallocExtension::GetStats();
    const std::string pbtxt = GetStatsInPbTxt();

#ifdef __x86_64__
    EXPECT_THAT(buf, HasSubstr(R"(PARAMETER hpaa_subrelease 0)"));
#endif
    EXPECT_THAT(buf,
                HasSubstr(R"(PARAMETER tcmalloc_guarded_sample_parameter -1)"));
    EXPECT_THAT(buf, HasSubstr(R"(PARAMETER tcmalloc_per_cpu_caches 0)"));
    EXPECT_THAT(buf,
                HasSubstr(R"(PARAMETER tcmalloc_max_per_cpu_cache_size -1)"));
    EXPECT_THAT(
        buf,
        HasSubstr(R"(PARAMETER tcmalloc_max_total_thread_cache_bytes -1)"));
    EXPECT_THAT(buf,
                HasSubstr(R"(PARAMETER tcmalloc_skip_subrelease_interval 1s)"));
    EXPECT_THAT(
        buf,
        HasSubstr(R"(PARAMETER tcmalloc_skip_subrelease_short_interval 2s)"));
    EXPECT_THAT(
        buf,
        HasSubstr(R"(PARAMETER tcmalloc_skip_subrelease_long_interval 3s)"));

#ifdef __x86_64__
    EXPECT_THAT(pbtxt, HasSubstr(R"(using_hpaa_subrelease: false)"));
#endif
    EXPECT_THAT(pbtxt, HasSubstr(R"(guarded_sample_parameter: -1)"));
    EXPECT_THAT(pbtxt, HasSubstr(R"(tcmalloc_per_cpu_caches: false)"));
    EXPECT_THAT(pbtxt, HasSubstr(R"(tcmalloc_max_per_cpu_cache_size: -1)"));
    EXPECT_THAT(pbtxt,
                HasSubstr(R"(tcmalloc_max_total_thread_cache_bytes: -1)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(R"(tcmalloc_skip_subrelease_interval_ns: 1000000000)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(R"(tcmalloc_skip_subrelease_short_interval_ns: 2000000000)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(R"(tcmalloc_skip_subrelease_long_interval_ns: 3000000000)"));
  }

#ifdef __x86_64__
  Parameters::set_hpaa_subrelease(true);
#endif
  Parameters::set_guarded_sampling_rate(50 *
                                        Parameters::profile_sampling_rate());
  Parameters::set_per_cpu_caches(true);
  Parameters::set_max_per_cpu_cache_size(3 << 20);
  Parameters::set_max_total_thread_cache_bytes(4 << 20);
  Parameters::set_filler_skip_subrelease_interval(absl::Milliseconds(60125));
  Parameters::set_filler_skip_subrelease_short_interval(
      absl::Milliseconds(120250));
  Parameters::set_filler_skip_subrelease_long_interval(
      absl::Milliseconds(180375));

  {
    const std::string buf = MallocExtension::GetStats();
    const std::string pbtxt = GetStatsInPbTxt();

#ifdef __x86_64__
    EXPECT_THAT(buf, HasSubstr(R"(PARAMETER hpaa_subrelease 1)"));
#endif
    EXPECT_THAT(buf,
                HasSubstr(R"(PARAMETER tcmalloc_guarded_sample_parameter 50)"));
    EXPECT_THAT(
        buf,
        HasSubstr(
            R"(PARAMETER desired_usage_limit_bytes 18446744073709551615)"));
    EXPECT_THAT(buf, HasSubstr(R"(PARAMETER tcmalloc_per_cpu_caches 1)"));
    EXPECT_THAT(
        buf, HasSubstr(R"(PARAMETER tcmalloc_max_per_cpu_cache_size 3145728)"));
    EXPECT_THAT(
        buf, HasSubstr(
                 R"(PARAMETER tcmalloc_max_total_thread_cache_bytes 4194304)"));
    EXPECT_THAT(
        buf,
        HasSubstr(R"(PARAMETER tcmalloc_skip_subrelease_interval 1m0.125s)"));
    EXPECT_THAT(
        buf,
        HasSubstr(
            R"(PARAMETER tcmalloc_skip_subrelease_short_interval 2m0.25s)"));
    EXPECT_THAT(
        buf,
        HasSubstr(
            R"(PARAMETER tcmalloc_skip_subrelease_long_interval 3m0.375s)"));

#ifdef __x86_64__
    EXPECT_THAT(pbtxt, HasSubstr(R"(using_hpaa_subrelease: true)"));
#endif
    EXPECT_THAT(pbtxt, HasSubstr(R"(guarded_sample_parameter: 50)"));
    EXPECT_THAT(pbtxt, HasSubstr(R"(desired_usage_limit_bytes: -1)"));
    EXPECT_THAT(pbtxt, HasSubstr(R"(hard_limit: false)"));
    EXPECT_THAT(pbtxt, HasSubstr(R"(tcmalloc_per_cpu_caches: true)"));
    EXPECT_THAT(pbtxt,
                HasSubstr(R"(tcmalloc_max_per_cpu_cache_size: 3145728)"));
    EXPECT_THAT(pbtxt,
                HasSubstr(R"(tcmalloc_max_total_thread_cache_bytes: 4194304)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(R"(tcmalloc_skip_subrelease_interval_ns: 60125000000)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(
            R"(tcmalloc_skip_subrelease_short_interval_ns: 120250000000)"));
    EXPECT_THAT(
        pbtxt,
        HasSubstr(
            R"(tcmalloc_skip_subrelease_long_interval_ns: 180375000000)"));
  }
}

}  // namespace
}  // namespace tcmalloc
