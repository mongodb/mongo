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
#include "tcmalloc/internal/percpu.h"

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syscall.h>
#include <unistd.h>

#include <atomic>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"  // IWYU pragma: keep
#include "absl/base/internal/sysinfo.h"
#include "tcmalloc/internal/linux_syscall_support.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/util.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace subtle {
namespace percpu {

// ----------------------------------------------------------------------------
// Internal structures
// ----------------------------------------------------------------------------

// Restartable Sequence (RSEQ)

extern "C" {
// We provide a per-thread value (defined in percpu_.c) which both tracks
// thread-local initialization state and (with RSEQ) provides an atomic
// in-memory reference for this thread's execution CPU.  This value is only
// valid when the thread is currently executing
// Possible values:
//   Unavailable/uninitialized:
//     { kCpuIdUnsupported, kCpuIdUninitialized }
//   Initialized, available:
//     [0, NumCpus())    (Always updated at context-switch)
ABSL_CONST_INIT thread_local volatile kernel_rseq __rseq_abi = {
    0,      static_cast<unsigned>(kCpuIdUninitialized),   0, 0,
    {0, 0}, {{kCpuIdUninitialized, kCpuIdUninitialized}},
};

}  // extern "C"

enum PerCpuInitStatus {
  kFastMode,
  kSlowMode,
};

ABSL_CONST_INIT static PerCpuInitStatus init_status = kSlowMode;
ABSL_CONST_INIT static absl::once_flag init_per_cpu_once;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
ABSL_CONST_INIT static std::atomic<bool> using_upstream_fence{false};
#endif  // TCMALLOC_INTERNAL_PERCPU_USE_RSEQ

// Is this thread's __rseq_abi struct currently registered with the kernel?
static bool ThreadRegistered() { return RseqCpuId() >= kCpuIdInitialized; }

static bool InitThreadPerCpu() {
  // If we're already registered, there's nothing further for us to do.
  if (ThreadRegistered()) {
    return true;
  }

#ifdef __NR_rseq
  return 0 == syscall(__NR_rseq, &__rseq_abi, sizeof(__rseq_abi), 0,
                      TCMALLOC_PERCPU_RSEQ_SIGNATURE);
#endif  // __NR_rseq
  return false;
}

bool UsingFlatVirtualCpus() {
  return false;
}

static void InitPerCpu() {
  CHECK_CONDITION(absl::base_internal::NumCPUs() <=
                  std::numeric_limits<uint16_t>::max());

  // Based on the results of successfully initializing the first thread, mark
  // init_status to initialize all subsequent threads.
  if (InitThreadPerCpu()) {
    init_status = kFastMode;

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
    constexpr int kMEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ = (1 << 8);
    // It is safe to make the syscall below multiple times.
    using_upstream_fence.store(
        0 == syscall(__NR_membarrier,
                     kMEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ, 0, 0),
        std::memory_order_relaxed);
#endif  // TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  }
}

// Tries to initialize RSEQ both at the process-wide (init_status) and
// thread-level (cpu-id) level.  If process-wide initialization has already been
// completed then only the thread-level will be completed.  A return of false
// indicates that initialization failed and RSEQ is unavailable.
bool InitFastPerCpu() {
  absl::base_internal::LowLevelCallOnce(&init_per_cpu_once, InitPerCpu);

  // Once we've decided fast-cpu support is available, initialization for all
  // subsequent threads must succeed for consistency.
  if (init_status == kFastMode && RseqCpuId() == kCpuIdUninitialized) {
    CHECK_CONDITION(InitThreadPerCpu());
  }

  // If we've decided to use slow mode, set the thread-local CPU ID to
  // __rseq_abi.cpu_id so that IsFast doesn't call this function again for
  // this thread.
  if (init_status == kSlowMode) {
    __rseq_abi.cpu_id = kCpuIdUnsupported;
  }

  return init_status == kFastMode;
}

// ----------------------------------------------------------------------------
// Implementation of unaccelerated (no RSEQ) per-cpu operations
// ----------------------------------------------------------------------------

static bool SetAffinityOneCpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (0 == sched_setaffinity(0, sizeof(cpu_set_t), &set)) {
    return true;
  }
  CHECK_CONDITION(errno == EINVAL);
  return false;
}

// We're being asked to fence against the mask <target>, but a -1 mask
// means every CPU.  Do we need <cpu>?
static bool NeedCpu(const int cpu, const int target) {
  return target == -1 || target == cpu;
}

static void SlowFence(int target) {
  // Necessary, so the point in time mentioned below has visibility
  // of our writes.
  std::atomic_thread_fence(std::memory_order_seq_cst);

  // First, save our cpumask (the user may want it back.)
  cpu_set_t old;
  CPU_ZERO(&old);
  CHECK_CONDITION(0 == sched_getaffinity(0, sizeof(cpu_set_t), &old));

  // Here's the basic idea: if we run on every CPU, then every thread
  // that runs after us has certainly seen every store we've made up
  // to this point, so we pin ourselves to each CPU in turn.
  //
  // But we can't run everywhere; our control plane may have set cpuset.cpus to
  // some subset of CPUs (and may be changing it as we speak.)  On the plus
  // side, if we are unable to run on a particular CPU, the same is true for our
  // siblings (up to some races, dealt with below), so we don't need to.

  for (int cpu = 0; cpu < absl::base_internal::NumCPUs(); ++cpu) {
    if (!NeedCpu(cpu, target)) {
      // unnecessary -- user doesn't care about synchronization on this cpu
      continue;
    }
    // If we can't pin ourselves there, then no one else can run there, so
    // that's fine.
    while (SetAffinityOneCpu(cpu)) {
      // But even if the pin succeeds, we might not end up running there;
      // between the pin trying to migrate and running on <cpu>, a change
      // to cpuset.cpus may cause us to migrate somewhere else instead.
      // So make sure we actually got where we wanted.
      if (cpu == sched_getcpu()) {
        break;
      }
    }
  }
  // Overly detailed explanation of kernel operations follows.
  //
  // OK, at this point, for each cpu i, there are two possibilities:
  //  * we've run on i (so we interrupted any sibling &  writes are visible)
  //  * At some point in time T1, we read a value of cpuset.cpus disallowing i.
  //
  // Linux kernel details: all writes and reads to cpuset.cpus are
  // serialized on a mutex (called callback_mutex).  Because of the
  // memory barrier above, our writes certainly happened-before T1.
  //
  // Moreover, whoever wrote cpuset.cpus to ban i looped over our
  // threads in kernel, migrating all threads away from i and setting
  // their masks to disallow i.  So once that loop is known to be
  // over, any thread that was running on i has been interrupted at
  // least once, and migrated away.  It is possible a second
  // subsequent change to cpuset.cpus (at time T2) re-allowed i, but
  // serialization of cpuset.cpus changes guarantee that our writes
  // are visible at T2, and since migration is a barrier, any sibling
  // migrated after T2 to cpu i will also see our writes.
  //
  // So we just have to make sure the update loop from whoever wrote
  // cpuset.cpus at T1 is completed.  That loop executes under a
  // second mutex (cgroup_mutex.)  So if we take that mutex ourselves,
  // we can be sure that update loop at T1 is done.  So read
  // /proc/self/cpuset. We don't care what it says; as long as it takes the lock
  // in question.  This guarantees that every thread is either running on a cpu
  // we visited, or received a cpuset.cpus rewrite that happened strictly after
  // our writes.

  using tcmalloc::tcmalloc_internal::signal_safe_close;
  using tcmalloc::tcmalloc_internal::signal_safe_open;
  using tcmalloc::tcmalloc_internal::signal_safe_read;
  int fd = signal_safe_open("/proc/self/cpuset", O_RDONLY);
  CHECK_CONDITION(fd >= 0);

  char c;
  CHECK_CONDITION(1 == signal_safe_read(fd, &c, 1, nullptr));

  CHECK_CONDITION(0 == signal_safe_close(fd));

  // Try to go back to what we originally had before Fence.
  if (0 != sched_setaffinity(0, sizeof(cpu_set_t), &old)) {
    CHECK_CONDITION(EINVAL == errno);
    // The original set is no longer valid, which should only happen if
    // cpuset.cpus was changed at some point in Fence.  If that happened and we
    // didn't fence, our control plane would have rewritten our affinity mask to
    // everything in cpuset.cpus, so do that.
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < absl::base_internal::NumCPUs(); ++i) {
      CPU_SET(i, &set);
    }
    CHECK_CONDITION(0 == sched_setaffinity(0, sizeof(cpu_set_t), &set));
  }
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
static void UpstreamRseqFenceCpu(int cpu) {
  ABSL_RAW_CHECK(using_upstream_fence.load(std::memory_order_relaxed),
                 "upstream fence unavailable.");

  constexpr int kMEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ = (1 << 7);
  constexpr int kMEMBARRIER_CMD_FLAG_CPU = (1 << 0);

  int64_t res = syscall(__NR_membarrier, kMEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
                        kMEMBARRIER_CMD_FLAG_CPU, cpu);

  ABSL_RAW_CHECK(res == 0 || res == -ENXIO /* missing CPU */,
                 "Upstream fence failed.");
}
#endif  // TCMALLOC_INTERNAL_PERCPU_USE_RSEQ

// Interrupt every concurrently running sibling thread on "cpu", and guarantee
// our writes up til now are visible to every other CPU. (cpu == -1 is
// equivalent to all CPUs.)
static void FenceInterruptCPU(int cpu) {
  CHECK_CONDITION(IsFast());

  // TODO(b/149390298):  Provide an upstream extension for sys_membarrier to
  // interrupt ongoing restartable sequences.
  SlowFence(cpu);
}

void FenceCpu(int cpu, const size_t virtual_cpu_id_offset) {
  // Prevent compiler re-ordering of code below. In particular, the call to
  // GetCurrentCpu must not appear in assembly program order until after any
  // code that comes before FenceCpu in C++ program order.
  CompilerBarrier();

  // A useful fast path: nothing needs doing at all to order us with respect
  // to our own CPU.
  if (ABSL_PREDICT_TRUE(IsFastNoInit()) &&
      GetCurrentVirtualCpu(virtual_cpu_id_offset) == cpu) {
    return;
  }

  if (virtual_cpu_id_offset == offsetof(kernel_rseq, vcpu_id)) {
    ASSUME(false);

    // With virtual CPUs, we cannot identify the true physical core we need to
    // interrupt.
    FenceAllCpus();
  }

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  if (using_upstream_fence.load(std::memory_order_relaxed)) {
    UpstreamRseqFenceCpu(cpu);
    return;
  }
#endif  // TCMALLOC_INTERNAL_PERCPU_USE_RSEQ

  FenceInterruptCPU(cpu);
}

void FenceAllCpus() {
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  if (using_upstream_fence.load(std::memory_order_relaxed)) {
    UpstreamRseqFenceCpu(-1);
    return;
  }
#endif  // TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  FenceInterruptCPU(-1);
}

}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
