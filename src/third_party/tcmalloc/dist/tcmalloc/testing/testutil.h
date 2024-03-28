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

#ifndef TCMALLOC_TESTING_TESTUTIL_H_
#define TCMALLOC_TESTING_TESTUTIL_H_

#include <sys/syscall.h>
#include <unistd.h>

#include <new>
#include <string>

#include "benchmark/benchmark.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/malloc_extension.h"

// When compiled 64-bit and run on systems with swap several unittests will end
// up trying to consume all of RAM+swap, and that can take quite some time.  By
// limiting the address-space size we get sufficient coverage without blowing
// out job limits.
void SetTestResourceLimit(size_t limit);
size_t GetTestResourceLimit();

namespace tcmalloc {

inline void sized_delete(void* ptr, size_t size) {
#ifdef __cpp_sized_deallocation
  ::operator delete(ptr, size);
#else
  (void)size;
  ::operator delete(ptr);
#endif
}

inline void sized_aligned_delete(void* ptr, size_t size,
                                 std::align_val_t alignment) {
#ifdef __cpp_sized_deallocation
  ::operator delete(ptr, size, alignment);
#else
  (void)size;
  ::operator delete(ptr, alignment);
#endif
}

inline void sized_array_delete(void* ptr, size_t size) {
#ifdef __cpp_sized_deallocation
  ::operator delete[](ptr, size);
#else
  (void)size;
  ::operator delete[](ptr);
#endif
}

inline void sized_array_aligned_delete(void* ptr, size_t size,
                                       std::align_val_t alignment) {
#ifdef __cpp_sized_deallocation
  ::operator delete[](ptr, size, alignment);
#else
  (void)size;
  ::operator delete[](ptr, alignment);
#endif
}

// Get the TCMalloc stats in textproto format.
std::string GetStatsInPbTxt();
extern "C" ABSL_ATTRIBUTE_WEAK int MallocExtension_Internal_GetStatsInPbtxt(
    char* buffer, int buffer_length);

class ScopedProfileSamplingRate {
 public:
  explicit ScopedProfileSamplingRate(int64_t temporary_value)
      : previous_(MallocExtension::GetProfileSamplingRate()) {
    MallocExtension::SetProfileSamplingRate(temporary_value);
    // Reset the per-thread sampler.  It may have a very large gap if sampling
    // had been disabled.
    void* ptr = ::operator new(256 * 1024 * 1024);
    // TODO(b/183453911): Remove workaround for GCC 10.x deleting operator new,
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94295.
    benchmark::DoNotOptimize(ptr);
    ::operator delete(ptr);
  }

  ~ScopedProfileSamplingRate() {
    MallocExtension::SetProfileSamplingRate(previous_);
    void* ptr = ::operator new(256 * 1024 * 1024);
    // TODO(b/183453911): Remove workaround for GCC 10.x deleting operator new,
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94295.
    benchmark::DoNotOptimize(ptr);
    ::operator delete(ptr);
  }

 private:
  int64_t previous_;
};

// Sets custom background thread actions enable/disable when in scope.
class ScopedBackgroundProcessActionsEnabled {
 public:
  explicit ScopedBackgroundProcessActionsEnabled(bool value)
      : previous_(MallocExtension::GetBackgroundProcessActionsEnabled()) {
    MallocExtension::SetBackgroundProcessActionsEnabled(value);
    if (!value) {
      // If the thread is doing something at the moment, let it finish.
      // This is unreliable, but simple. The thread sleeps for 1 sec,
      // so we sleep for 5.
      absl::SleepFor(absl::Seconds(5));
    }
  }

  ~ScopedBackgroundProcessActionsEnabled() {
    MallocExtension::SetBackgroundProcessActionsEnabled(previous_);
  }

 private:
  bool previous_;
};

// Sets a custom background thread process sleep interval when in scope.
class ScopedBackgroundProcessSleepInterval {
 public:
  explicit ScopedBackgroundProcessSleepInterval(absl::Duration limit)
      : previous_(MallocExtension::GetBackgroundProcessSleepInterval()) {
    MallocExtension::SetBackgroundProcessSleepInterval(limit);
  }

  ~ScopedBackgroundProcessSleepInterval() {
    MallocExtension::SetBackgroundProcessSleepInterval(previous_);
  }

 private:
  absl::Duration previous_;
};

// Sets a custom resource limit when in scope.
class ScopedResourceLimit {
 public:
  explicit ScopedResourceLimit(size_t limit)
      : previous_(GetTestResourceLimit()) {
    SetTestResourceLimit(limit);
  }

  ~ScopedResourceLimit() { SetTestResourceLimit(previous_); }

 private:
  size_t previous_;
};

class ScopedGuardedSamplingRate {
 public:
  explicit ScopedGuardedSamplingRate(int64_t temporary_value)
      : previous_(MallocExtension::GetGuardedSamplingRate()) {
    MallocExtension::SetGuardedSamplingRate(temporary_value);
  }

  ~ScopedGuardedSamplingRate() {
    MallocExtension::SetGuardedSamplingRate(previous_);
  }

 private:
  int64_t previous_;
};

// Disables both guarded sampling and profile sampling while in scope.
class ScopedNeverSample {
 public:
  ScopedNeverSample() : guarded_sampling_rate_(-1), profile_sampling_rate_(0) {}

 private:
  ScopedGuardedSamplingRate guarded_sampling_rate_;
  ScopedProfileSamplingRate profile_sampling_rate_;
};

// Enables both guarded sampling and profile sampling for all allocations while
// in scope.
class ScopedAlwaysSample {
 public:
  // See b/201336703: guarded_sampling_rate==0 means every sampled allocation
  // is guarded.
  ScopedAlwaysSample() : guarded_sampling_rate_(0), profile_sampling_rate_(1) {}

 private:
  ScopedGuardedSamplingRate guarded_sampling_rate_;
  ScopedProfileSamplingRate profile_sampling_rate_;
};

inline void UnregisterRseq() {
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  syscall(__NR_rseq, &tcmalloc_internal::subtle::percpu::__rseq_abi,
          sizeof(tcmalloc_internal::subtle::percpu::__rseq_abi),
          tcmalloc_internal::subtle::percpu::kRseqUnregister,
          TCMALLOC_PERCPU_RSEQ_SIGNATURE);
#else
  TC_BUG("rseq is is unavailable in this build");
#endif
}

// ScopedUnregisterRseq unregisters the current thread from rseq.  On
// destruction, it reregisters it with IsFast().
class ScopedUnregisterRseq {
 public:
  ScopedUnregisterRseq() {
    // Since we expect that we will be able to register the thread for rseq in
    // the destructor, verify that we can do so now.
    TC_CHECK(tcmalloc_internal::subtle::percpu::IsFast());

    UnregisterRseq();

    // Unregistering stores kCpuIdUninitialized to the cpu_id field.
    TC_CHECK_EQ(tcmalloc_internal::subtle::percpu::RseqCpuId(),
                tcmalloc_internal::subtle::percpu::kCpuIdUninitialized);
  }

  // REQUIRES: __rseq_abi.cpu_id == kCpuIdUninitialized
  ~ScopedUnregisterRseq() {
    TC_CHECK(tcmalloc_internal::subtle::percpu::IsFast());
  }
};

// An RAII object that injects a fake CPU ID into the percpu library for as long
// as it exists.
class ScopedFakeCpuId {
 public:
  explicit ScopedFakeCpuId(const int cpu_id) {
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
    // Now that our unregister_rseq_ member has prevented the kernel from
    // modifying __rseq_abi, we can inject our own CPU ID.
    tcmalloc_internal::subtle::percpu::__rseq_abi.cpu_id = cpu_id;

    if (tcmalloc_internal::subtle::percpu::UsingFlatVirtualCpus()) {
      tcmalloc_internal::subtle::percpu::__rseq_abi.vcpu_id = cpu_id;
    }
#endif
  }

  ~ScopedFakeCpuId() {
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
    // Undo the modification we made in the constructor, as required by
    // ~ScopedFakeCpuId.
    tcmalloc_internal::subtle::percpu::__rseq_abi.cpu_id =
        tcmalloc_internal::subtle::percpu::kCpuIdUninitialized;

    if (tcmalloc_internal::subtle::percpu::UsingFlatVirtualCpus()) {
      tcmalloc_internal::subtle::percpu::__rseq_abi.vcpu_id =
          tcmalloc_internal::subtle::percpu::kCpuIdUninitialized;
    }
#endif
  }

 private:

  const ScopedUnregisterRseq unregister_rseq_;
};

// This pragma ensures that a loop does not get unrolled, in which case the
// different loop iterations would map to different call sites instead of the
// same ones as expected by some tests. Supported pragmas differ between GCC and
// Clang, which is why we need this conditional.
#if (defined(__clang__) || defined(__INTEL_COMPILER))
#define PRAGMA_NO_UNROLL _Pragma("nounroll")
#elif (defined(__GNUC__) || defined(__GCUG__))
// GCC does not always respect "#pragma unroll <N>". The most reliable approach
// is therefore to completely disable optimizations for this source file.
#pragma GCC optimize("O0")
#define PRAGMA_NO_UNROLL
#else
// If #pragma nounroll is unsupported, the test may still work by compiling with
// equivalent compiler options.
#define PRAGMA_NO_UNROLL
#endif

// Tests if the array of doubles is uniformly distributed.
// Returns the p-value of the Anderson Darling Statistic
// for the given set of sorted random doubles
// See "Evaluating the Anderson-Darling Distribution" by
// Marsaglia and Marsaglia for details.
double AndersonDarlingTest(absl::Span<const double> random_sample);

}  // namespace tcmalloc

#endif  // TCMALLOC_TESTING_TESTUTIL_H_
