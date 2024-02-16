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

#include "tcmalloc/internal/affinity.h"

#include <errno.h>
#include <sched.h>

#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/percpu.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

static bool AffinityMatches(std::vector<int> expected_affinity) {
  cpu_set_t allowed_cpus;
  EXPECT_EQ(sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus), 0)
      << errno;
  for (int cpu : expected_affinity) {
    if (!CPU_ISSET(cpu, &allowed_cpus)) return false;

    CPU_CLR(cpu, &allowed_cpus);
  }

  // All cpus should now be accounted for.
  return CPU_COUNT(&allowed_cpus) == 0;
}

TEST(AffinityInternalTest, AllowedCpus) {
  ASSERT_THAT(AllowedCpus(),
              testing::Contains(subtle::percpu::GetCurrentCpu()));
  ASSERT_TRUE(AffinityMatches(AllowedCpus()));
}

TEST(AffinityInternalTest, ScopedAffinityTamper) {
  // It would be convenient to use a ScopedAffinityMask here also, however, the
  // tamper logic disables the destructor (this is intentional so as to leave us
  // with the most consistent masks).
  cpu_set_t original_cpus;
restart:
  EXPECT_EQ(sched_getaffinity(0, sizeof(original_cpus), &original_cpus), 0)
      << errno;

  // We require at least 2 cpus to run this test.
  if (CPU_COUNT(&original_cpus) == 1) return;

  for (int i = 0; i < CPU_SETSIZE; i++) {
    if (CPU_ISSET(i, &original_cpus)) {
      ScopedAffinityMask mask(i);

      // Progressing past this point _requires_ a successful false return.
      if (mask.Tampered()) goto restart;

      EXPECT_FALSE(mask.Tampered());
      // Manually tamper.  Note that the only way this can fail (external
      // restriction away from "i", must in itself trigger tampering.
      sched_setaffinity(0, sizeof(original_cpus), &original_cpus);
      ASSERT_TRUE(mask.Tampered());
      break;
    }
  }
  // We already restored original_cpus above.
}

TEST(AffinityInternalTest, ScopedAffinityMask) {
  auto original_cpus = AllowedCpus();

restart:
  std::vector<int> original_affinity = AllowedCpus(), temporary_affinity;

  for (int i = 0; i < original_affinity.size(); i++) {
    if (AllowedCpus() != original_affinity) goto restart;

    temporary_affinity.push_back(original_affinity[i]);
    ScopedAffinityMask mask(absl::MakeSpan(temporary_affinity));
    ASSERT_TRUE(AllowedCpus() == temporary_affinity || mask.Tampered());

    if (mask.Tampered()) {
      goto restart;
    }
  }

  EXPECT_EQ(original_affinity, AllowedCpus());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
