// Copyright 2024 The TCMalloc Authors
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

#include "tcmalloc/internal/percpu.h"

#include <sys/time.h>

#include <atomic>
#include <cstring>

#include "gtest/gtest.h"
#include "absl/base/attributes.h"
#include "absl/log/absl_check.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc::tcmalloc_internal::subtle::percpu {
namespace {

ABSL_CONST_INIT std::atomic<int> alarms{0};

void sa_alrm(int sig) {
  alarms.fetch_add(1, std::memory_order_relaxed);
  TC_CHECK(IsFast());
}

TEST(PerCpu, SignalHandling) {
  if (!IsFast()) {
    GTEST_SKIP() << "per-CPU unavailable";
  }

  struct sigaction sig;
  memset(&sig, 0, sizeof(sig));  // sa_flags == 0 => SA_RESTART not set
  sig.sa_handler = sa_alrm;
  ABSL_CHECK_EQ(sigaction(SIGALRM, &sig, nullptr),
                0);  // install signal handler

  constexpr absl::Duration interval = absl::Microseconds(1);
  struct timeval timeval = absl::ToTimeval(interval);

  struct itimerval signal_interval;
  signal_interval.it_value = timeval;
  signal_interval.it_interval = timeval;

  setitimer(ITIMER_REAL, &signal_interval, nullptr);

  for (int i = 0; i < 100000; ++i) {
    UnregisterRseq();
    TC_CHECK(IsFast());
  }

  timeval = absl::ToTimeval(absl::ZeroDuration());
  signal_interval.it_value = timeval;
  signal_interval.it_interval = timeval;

  setitimer(ITIMER_REAL, &signal_interval, nullptr);

  EXPECT_GT(alarms.load(std::memory_order_relaxed), 0);
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal::subtle::percpu
