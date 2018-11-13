// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <vector>

#include "benchmark/benchmark.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/synchronization/blocking_counter.h"
#include "absl/synchronization/internal/thread_pool.h"
#include "absl/synchronization/mutex.h"

namespace {

// Measure the overhead of conditions on mutex release (when they must be
// evaluated).  Mutex has (some) support for equivalence classes allowing
// Conditions with the same function/argument to potentially not be multiply
// evaluated.
//
// num_classes==0 is used for the special case of every waiter being distinct.
void BM_ConditionWaiters(benchmark::State& state) {
  int num_classes = state.range(0);
  int num_waiters = state.range(1);

  struct Helper {
    static void Waiter(absl::BlockingCounter* init, absl::Mutex* m, int* p) {
      init->DecrementCount();
      m->LockWhen(absl::Condition(
          static_cast<bool (*)(int*)>([](int* v) { return *v == 0; }), p));
      m->Unlock();
    }
  };

  if (num_classes == 0) {
    // No equivalence classes.
    num_classes = num_waiters;
  }

  absl::BlockingCounter init(num_waiters);
  absl::Mutex mu;
  std::vector<int> equivalence_classes(num_classes, 1);

  // Must be declared last to be destroyed first.
  absl::synchronization_internal::ThreadPool pool(num_waiters);

  for (int i = 0; i < num_waiters; i++) {
    // Mutex considers Conditions with the same function and argument
    // to be equivalent.
    pool.Schedule([&, i] {
      Helper::Waiter(&init, &mu, &equivalence_classes[i % num_classes]);
    });
  }
  init.Wait();

  for (auto _ : state) {
    mu.Lock();
    mu.Unlock();  // Each unlock requires Condition evaluation for our waiters.
  }

  mu.Lock();
  for (int i = 0; i < num_classes; i++) {
    equivalence_classes[i] = 0;
  }
  mu.Unlock();
}

// Some configurations have higher thread limits than others.
#if defined(__linux__) && !defined(THREAD_SANITIZER)
constexpr int kMaxConditionWaiters = 8192;
#else
constexpr int kMaxConditionWaiters = 1024;
#endif
BENCHMARK(BM_ConditionWaiters)->RangePair(0, 2, 1, kMaxConditionWaiters);

void BM_ContendedMutex(benchmark::State& state) {
  static absl::Mutex* mu = new absl::Mutex;
  for (auto _ : state) {
    absl::MutexLock lock(mu);
  }
}
BENCHMARK(BM_ContendedMutex)->Threads(1);
BENCHMARK(BM_ContendedMutex)->ThreadPerCpu();

}  // namespace
