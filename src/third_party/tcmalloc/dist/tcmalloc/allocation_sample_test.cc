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

#include "tcmalloc/allocation_sample.h"

#include <stddef.h>

#include <memory>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/thread_annotations.h"
#include "absl/random/bit_gen_ref.h"
#include "absl/random/random.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/testing/thread_manager.h"

namespace tcmalloc::tcmalloc_internal {
namespace {

TEST(AllocationSample, Threaded) {
  // StackTraceTable uses a global allocator.  It must be initialized.
  tc_globals.InitIfNecessary();

  // This test exercises b/143623146 by ensuring that the state of the sample is
  // not modified before it is removed from the linked list.
  AllocationSampleList list;

  const int kThreads = 5;
  const int kMaxSamplers = 3;
  const int kMaxAllocations = 100;
  ThreadManager m;
  std::vector<absl::BitGen> thread_states(kThreads);

  struct GlobalState {
    absl::Mutex mu;
    std::vector<std::unique_ptr<AllocationSample>> samplers ABSL_GUARDED_BY(mu);
  } global;

  auto PopSample = [&](absl::BitGenRef rng) {
    std::unique_ptr<AllocationSample> ret;

    // Do our test bookkeeping separately, so we don't synchronize list
    // externally.
    absl::MutexLock l(&global.mu);
    if (global.samplers.empty()) {
      return ret;
    }
    size_t index = absl::Uniform<size_t>(rng, 0, global.samplers.size() - 1u);
    std::swap(global.samplers[index], global.samplers.back());
    ret = std::move(global.samplers.back());
    global.samplers.pop_back();

    CHECK_CONDITION(ret != nullptr);
    return ret;
  };

  m.Start(kThreads, [&](int thread) {
    auto& state = thread_states[thread];
    const double coin = absl::Uniform(state, 0., 1.0);

    if (coin < 0.1) {
      // Add a sampler.  This occurs implicitly in the AllocationSample
      // constructor.
      auto sampler = std::make_unique<AllocationSample>(&list, absl::Now());

      // Do our test bookkeeping separately, so we don't synchronize list
      // externally.
      {
        absl::MutexLock l(&global.mu);
        if (global.samplers.size() < kMaxSamplers) {
          // Add to the list.
          global.samplers.push_back(std::move(sampler));
        }
      }

      // If we didn't push it, we will unregister in ~AllocationSample.
    } else if (coin < 0.2) {
      std::unique_ptr<AllocationSample> sampler = PopSample(state);

      // Remove a sample and allow its destructor to handle unregistering.
      sampler.reset();
    } else if (coin < 0.25) {
      // Call Stop occasionally.
      std::unique_ptr<AllocationSample> sampler = PopSample(state);

      if (sampler) {
        std::move(*sampler).Stop();
      }
    } else {
      int allocations;
      {
        // StackTraceTable uses a global allocator, rather than one that is
        // injected.  Consult the global state to see how many allocations are
        // active.
        absl::base_internal::SpinLockHolder h(&pageheap_lock);
        allocations = tc_globals.linked_sample_allocator().stats().in_use;
      }
      if (allocations >= kMaxAllocations) {
        return;
      }

      StackTrace s{};
      s.requested_size = 16;
      s.allocated_size = 32;
      list.ReportMalloc(s);
    }
  });

  absl::SleepFor(absl::Milliseconds(1));

  m.Stop();
}

}  // namespace
}  // namespace tcmalloc::tcmalloc_internal
