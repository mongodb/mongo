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
//
// A few routines that are useful for multiple tests in this directory.

#include "tcmalloc/testing/testutil.h"

#include <pthread.h>
#include <stdlib.h>
#include <sys/resource.h>

#include <cstring>
#include <limits>

// When compiled 64-bit and run on systems with swap several unittests will end
// up trying to consume all of RAM+swap, and that can take quite some time.  By
// limiting the address-space size we get sufficient coverage without blowing
// out job limits.
void SetTestResourceLimit() {
  // The actual resource we need to set varies depending on which flavour of
  // unix.  On Linux we need RLIMIT_AS because that covers the use of mmap.
  // Otherwise hopefully RLIMIT_RSS is good enough.  (Unfortunately 64-bit
  // and 32-bit headers disagree on the type of these constants!)
#ifdef RLIMIT_AS
#define USE_RESOURCE RLIMIT_AS
#else
#define USE_RESOURCE RLIMIT_RSS
#endif

  // Restrict the test to 8GiB by default.
  // Be careful we don't overflow rlim - if we would, this is a no-op
  // and we can just do nothing.
  const int64_t lim = static_cast<int64_t>(8) * 1024 * 1024 * 1024;
  if (lim > std::numeric_limits<rlim_t>::max()) return;
  const rlim_t kMaxMem = lim;

  struct rlimit rlim;
  if (getrlimit(USE_RESOURCE, &rlim) == 0) {
    if (rlim.rlim_cur == RLIM_INFINITY || rlim.rlim_cur > kMaxMem) {
      rlim.rlim_cur = kMaxMem;
      setrlimit(USE_RESOURCE, &rlim);  // ignore result
    }
  }
}

namespace tcmalloc {

std::string GetStatsInPbTxt() {
  // When huge page telemetry is enabled, the output can become very large.
  const int buffer_length = 3 << 20;
  std::string buf;
  if (&MallocExtension_Internal_GetStatsInPbtxt == nullptr) {
    return buf;
  }

  buf.resize(buffer_length);
  int actual_size =
      MallocExtension_Internal_GetStatsInPbtxt(&buf[0], buffer_length);
  buf.resize(actual_size);
  return buf;
}

}  // namespace tcmalloc
