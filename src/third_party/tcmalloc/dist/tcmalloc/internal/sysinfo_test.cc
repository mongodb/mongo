// Copyright 2023 The TCMalloc Authors
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

#include "tcmalloc/internal/sysinfo.h"

#include <sched.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "tcmalloc/internal/allocation_guard.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(ParseCpulistTest, Empty) {
  absl::string_view empty("\n");

  const absl::optional<cpu_set_t> parsed =
      ParseCpulist([&](char* const buf, const size_t count) -> ssize_t {
        // Calculate how much data we have left to provide.
        const size_t to_copy = std::min(count, empty.size());

        // If none, we have no choice but to provide nothing.
        if (to_copy == 0) return 0;

        memcpy(buf, empty.data(), to_copy);
        empty.remove_prefix(to_copy);
        return to_copy;
      });

  // No CPUs should be active on this NUMA node.
  ASSERT_THAT(parsed, testing::Ne(std::nullopt));
  EXPECT_EQ(CPU_COUNT(&*parsed), 0);
}

TEST(ParseCpulistTest, NotInBounds) {
  std::string cpulist = absl::StrCat("0-", CPU_SETSIZE);

  const absl::optional<cpu_set_t> parsed =
      ParseCpulist([&](char* const buf, const size_t count) -> ssize_t {
        // Calculate how much data we have left to provide.
        const size_t to_copy = std::min(count, cpulist.size());

        // If none, we have no choice but to provide nothing.
        if (to_copy == 0) return 0;

        memcpy(buf, cpulist.data(), to_copy);
        cpulist.erase(0, to_copy);
        return to_copy;
      });

  ASSERT_THAT(parsed, testing::Eq(std::nullopt));
}

// Ensure that we can parse randomized cpulists correctly.
TEST(ParseCpulistTest, Random) {
  absl::BitGen gen;

  static constexpr int kIterations = 100;
  for (int i = 0; i < kIterations; i++) {
    cpu_set_t reference;
    CPU_ZERO(&reference);

    // Set a random number of CPUs within the reference set.
    const double density = absl::Uniform(gen, 0.0, 1.0);
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
      if (absl::Bernoulli(gen, density)) {
        CPU_SET(cpu, &reference);
      }
    }

    // Serialize the reference set into a cpulist-style string.
    std::vector<std::string> components;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
      if (!CPU_ISSET(cpu, &reference)) continue;

      const int start = cpu;
      int next = cpu + 1;
      while (next < CPU_SETSIZE && CPU_ISSET(next, &reference)) {
        cpu = next;
        next = cpu + 1;
      }

      if (cpu == start) {
        components.push_back(absl::StrCat(cpu));
      } else {
        components.push_back(absl::StrCat(start, "-", cpu));
      }
    }
    const std::string serialized = absl::StrJoin(components, ",");

    // Now parse that string using our ParseCpulist function, randomizing the
    // amount of data we provide to it from each read.
    absl::string_view remaining(serialized);
    const absl::optional<cpu_set_t> parsed =
        ParseCpulist([&](char* const buf, const size_t count) -> ssize_t {
          // Calculate how much data we have left to provide.
          const size_t max = std::min(count, remaining.size());

          // If none, we have no choice but to provide nothing.
          if (max == 0) return 0;

          // If we do have data, return a randomly sized subset of it to stress
          // the logic around reading partial values.
          const size_t copy = absl::Uniform(gen, static_cast<size_t>(1), max);
          memcpy(buf, remaining.data(), copy);
          remaining.remove_prefix(copy);
          return copy;
        });

    // We ought to have parsed the same set of CPUs that we serialized.
    ASSERT_THAT(parsed, testing::Ne(std::nullopt));
    EXPECT_TRUE(CPU_EQUAL(&*parsed, &reference));
  }
}

TEST(NumCPUs, NoCache) {
  const int result = []() {
    AllocationGuard guard;
    return sysinfo_internal::NumPossibleCPUsNoCache();
  }();

  // TODO(b/67389555): This test may fail if there are offlined CPUs.
  EXPECT_EQ(result, absl::base_internal::NumCPUs());
}

TEST(NumCPUs, Cached) {
  // TODO(b/67389555): This test may fail if there are offlined CPUs.
  EXPECT_EQ(NumCPUs(), absl::base_internal::NumCPUs());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
