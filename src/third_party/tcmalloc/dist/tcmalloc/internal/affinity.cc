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
#include "tcmalloc/internal/affinity.h"

#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>

#include <vector>

#include "absl/types/span.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

std::vector<int> AllowedCpus() {
  // We have no need for dynamically sized sets (currently >1024 CPUs for glibc)
  // at the present time.  We could change this in the future.
  cpu_set_t allowed_cpus;
  TC_CHECK_EQ(0, sched_getaffinity(0, sizeof(allowed_cpus), &allowed_cpus));
  int n = CPU_COUNT(&allowed_cpus), c = 0;

  std::vector<int> result(n);
  for (int i = 0; i < CPU_SETSIZE && n; i++) {
    if (CPU_ISSET(i, &allowed_cpus)) {
      result[c++] = i;
      n--;
    }
  }
  TC_CHECK_EQ(0, n);

  return result;
}

static cpu_set_t SpanToCpuSetT(absl::Span<int> mask) {
  cpu_set_t result;
  CPU_ZERO(&result);
  for (int cpu : mask) {
    CPU_SET(cpu, &result);
  }
  return result;
}

ScopedAffinityMask::ScopedAffinityMask(absl::Span<int> allowed_cpus) {
  specified_cpus_ = SpanToCpuSetT(allowed_cpus);
  // getaffinity should never fail.
  TC_CHECK_EQ(0, sched_getaffinity(0, sizeof(original_cpus_), &original_cpus_));
  // See destructor comments on setaffinity interactions.  Tampered() will
  // necessarily be true in this case.
  sched_setaffinity(0, sizeof(specified_cpus_), &specified_cpus_);
}

ScopedAffinityMask::ScopedAffinityMask(int allowed_cpu) {
  CPU_ZERO(&specified_cpus_);
  CPU_SET(allowed_cpu, &specified_cpus_);

  // getaffinity should never fail.
  TC_CHECK_EQ(0, sched_getaffinity(0, sizeof(original_cpus_), &original_cpus_));
  // See destructor comments on setaffinity interactions.  Tampered() will
  // necessarily be true in this case.
  sched_setaffinity(0, sizeof(specified_cpus_), &specified_cpus_);
}

ScopedAffinityMask::~ScopedAffinityMask() {
  // If something else has already reset our affinity, do not attempt to
  // restrict towards our original mask.  This is best-effort as the tampering
  // may obviously occur during the destruction of *this.
  if (!Tampered()) {
    // Note:  We do not assert success here, conflicts may restrict us from all
    // 'original_cpus_'.
    sched_setaffinity(0, sizeof(original_cpus_), &original_cpus_);
  }
}

bool ScopedAffinityMask::Tampered() {
  cpu_set_t current_cpus;
  TC_CHECK_EQ(0, sched_getaffinity(0, sizeof(current_cpus), &current_cpus));
  return !CPU_EQUAL(&current_cpus, &specified_cpus_);  // Mismatch => modified.
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
