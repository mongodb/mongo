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

#ifndef TCMALLOC_INTERNAL_AFFINITY_H_
#define TCMALLOC_INTERNAL_AFFINITY_H_

#include <sched.h>

#include <vector>

#include "absl/types/span.h"
#include "tcmalloc/internal/config.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Affinity helpers.

// Returns a vector of the which cpus the currently allowed thread is allowed to
// run on.  There are no guarantees that this will not change before, after, or
// even during, the call to AllowedCpus().
std::vector<int> AllowedCpus();

// Enacts a scoped affinity mask on the constructing thread.  Attempts to
// restore the original affinity mask on destruction.
//
// REQUIRES: For test-use only.  Do not use this in production code.
class ScopedAffinityMask {
 public:
  // When racing with an external restriction that has a zero-intersection with
  // "allowed_cpus" we will construct, but immediately register as "Tampered()",
  // without actual changes to affinity.
  explicit ScopedAffinityMask(absl::Span<int> allowed_cpus);
  explicit ScopedAffinityMask(int allowed_cpu);

  // Restores original affinity iff our scoped affinity has not been externally
  // modified (i.e. Tampered()).  Otherwise, the updated affinity is preserved.
  ~ScopedAffinityMask();

  // Returns true if the affinity mask no longer matches what was set at point
  // of construction.
  //
  // Note:  This is instantaneous and not fool-proof.  It's possible for an
  // external affinity modification to subsequently align with our originally
  // specified "allowed_cpus".  In this case Tampered() will return false when
  // time may have been spent executing previously on non-specified cpus.
  bool Tampered();

 private:
  cpu_set_t original_cpus_, specified_cpus_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_AFFINITY_H_
