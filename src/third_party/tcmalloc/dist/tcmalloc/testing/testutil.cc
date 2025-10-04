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

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "tcmalloc/internal/logging.h"

// When compiled 64-bit and run on systems with swap several unittests will end
// up trying to consume all of RAM+swap, and that can take quite some time.  By
// limiting the address-space size we get sufficient coverage without blowing
// out job limits.
void SetTestResourceLimit(size_t limit) {
  // The actual resource we need to set varies depending on which flavour of
  // unix.  On Linux we need RLIMIT_AS because that covers the use of mmap.
  // Otherwise hopefully RLIMIT_RSS is good enough.  (Unfortunately 64-bit
  // and 32-bit headers disagree on the type of these constants!)
#ifdef RLIMIT_AS
#define USE_RESOURCE RLIMIT_AS
#else
#define USE_RESOURCE RLIMIT_RSS
#endif

#ifdef TCMALLOC_INTERNAL_SELSAN
  return;  // SelSan allocates lots of virtual memory.
#endif

  // Be careful we don't overflow rlim - if we would, this is a no-op
  // and we can just do nothing.
  const int64_t lim = static_cast<int64_t>(limit);
  if (lim > std::numeric_limits<rlim_t>::max()) return;
  const rlim_t kMaxMem = lim;

  struct rlimit rlim;
  if (getrlimit(USE_RESOURCE, &rlim) == 0) {
    rlim.rlim_cur = kMaxMem;
    setrlimit(USE_RESOURCE, &rlim);  // ignore result
  }
}

// Fetch the current resource limit applied to the test job.
size_t GetTestResourceLimit() {
  // The actual resource we need to set varies depending on which flavour of
  // unix.  On Linux we need RLIMIT_AS because that covers the use of mmap.
  // Otherwise hopefully RLIMIT_RSS is good enough.  (Unfortunately 64-bit
  // and 32-bit headers disagree on the type of these constants!)
#ifdef RLIMIT_AS
#define USE_RESOURCE RLIMIT_AS
#else
#define USE_RESOURCE RLIMIT_RSS
#endif

  struct rlimit rlim;
  TC_CHECK_EQ(0, getrlimit(USE_RESOURCE, &rlim));
  return rlim.rlim_cur;
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

// Short cut version of ADinf(z), z>0 (from Marsaglia)
// This returns the p-value for Anderson Darling statistic in
// the limit as n-> infinity. For finite n, apply the error fix below.
double AndersonDarlingInf(double z) {
  if (z < 2) {
    return exp(-1.2337141 / z) / sqrt(z) *
           (2.00012 +
            (0.247105 -
             (0.0649821 - (0.0347962 - (0.011672 - 0.00168691 * z) * z) * z) *
                 z) *
                z);
  }
  return exp(
      -exp(1.0776 -
           (2.30695 -
            (0.43424 - (0.082433 - (0.008056 - 0.0003146 * z) * z) * z) * z) *
               z));
}

// Corrects the approximation error in AndersonDarlingInf for small values of n
// Add this to AndersonDarlingInf to get a better approximation
// (from Marsaglia)
double AndersonDarlingErrFix(int n, double x) {
  if (x > 0.8) {
    return (-130.2137 +
            (745.2337 -
             (1705.091 - (1950.646 - (1116.360 - 255.7844 * x) * x) * x) * x) *
                x) /
           n;
  }
  double cutoff = 0.01265 + 0.1757 / n;
  double t;
  if (x < cutoff) {
    t = x / cutoff;
    t = sqrt(t) * (1 - t) * (49 * t - 102);
    return t * (0.0037 / (n * n) + 0.00078 / n + 0.00006) / n;
  } else {
    t = (x - cutoff) / (0.8 - cutoff);
    t = -0.00022633 +
        (6.54034 - (14.6538 - (14.458 - (8.259 - 1.91864 * t) * t) * t) * t) *
            t;
    return t * (0.04213 + 0.01365 / n) / n;
  }
}

// Returns the AndersonDarling p-value given n and the value of the statistic
double AndersonDarlingPValue(int n, double z) {
  double ad = AndersonDarlingInf(z);
  double errfix = AndersonDarlingErrFix(n, ad);
  return ad + errfix;
}

double AndersonDarlingStatistic(absl::Span<const double> random_sample) {
  int n = random_sample.size();
  double ad_sum = 0;
  for (int i = 0; i < n; i++) {
    ad_sum += (2 * i + 1) *
              std::log(random_sample[i] * (1 - random_sample[n - 1 - i]));
  }
  double ad_statistic = -n - 1 / static_cast<double>(n) * ad_sum;
  return ad_statistic;
}

// Tests if the array of doubles is uniformly distributed.
// Returns the p-value of the Anderson Darling Statistic
// for the given set of sorted random doubles
// See "Evaluating the Anderson-Darling Distribution" by
// Marsaglia and Marsaglia for details.
double AndersonDarlingTest(absl::Span<const double> random_sample) {
  double ad_statistic = AndersonDarlingStatistic(random_sample);
  double p = AndersonDarlingPValue(random_sample.size(), ad_statistic);
  return p;
}

}  // namespace tcmalloc
