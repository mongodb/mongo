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
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <limits>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"  // IWYU pragma: keep
#include "absl/base/optimization.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/linux_syscall_support.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/sysinfo.h"
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

enum PerCpuInitStatus {
  kFastMode,
  kSlowMode,
};

ABSL_CONST_INIT static PerCpuInitStatus init_status = kSlowMode;
ABSL_CONST_INIT static absl::once_flag init_per_cpu_once;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
ABSL_CONST_INIT static std::atomic<bool> using_upstream_fence{false};
#endif  // TCMALLOC_INTERNAL_PERCPU_USE_RSEQ

extern "C" thread_local char tcmalloc_sampler ABSL_ATTRIBUTE_INITIAL_EXEC;

// Is this thread's __rseq_abi struct currently registered with the kernel?
static bool ThreadRegistered() { return RseqCpuId() >= kCpuIdInitialized; }

static bool InitThreadPerCpu() {
  // If we're already registered, there's nothing further for us to do.
  if (ThreadRegistered()) {
    return true;
  }

  // Mask signals and double check thread registration afterwards.  If we
  // encounter a signal between ThreadRegistered() above and rseq() and that
  // signal initializes per-CPU, rseq() here will fail with EBUSY.
  ScopedSigmask mask;

  if (ThreadRegistered()) {
    return true;
  }

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__NR_rseq)
  return 0 == syscall(__NR_rseq, &__rseq_abi, sizeof(__rseq_abi), 0,
                      TCMALLOC_PERCPU_RSEQ_SIGNATURE);
#endif  // __NR_rseq
  return false;
}

bool UsingFlatVirtualCpus() {
  return false;
}

static void InitPerCpu() {
  TC_CHECK(NumCPUs() <= std::numeric_limits<uint16_t>::max());

  // Based on the results of successfully initializing the first thread, mark
  // init_status to initialize all subsequent threads.
  if (InitThreadPerCpu()) {
    init_status = kFastMode;

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
    // See the comment about data layout in percpu.h for details.
    auto sampler_addr = reinterpret_cast<uintptr_t>(&tcmalloc_sampler);
    // Have to use volatile because C++ compiler rejects to believe that
    // objects can overlap.
    volatile auto slabs_addr = reinterpret_cast<uintptr_t>(&tcmalloc_slabs);
    auto rseq_abi_addr = reinterpret_cast<uintptr_t>(&__rseq_abi);
    //  Ensure __rseq_abi alignment required by ABI.
    TC_CHECK_EQ(rseq_abi_addr % 32, 0);
    // Ensure that all our TLS data is in a single cache line.
    TC_CHECK_EQ(rseq_abi_addr / 64, slabs_addr / 64);
    TC_CHECK_EQ(rseq_abi_addr / 64,
                (sampler_addr + TCMALLOC_SAMPLER_HOT_OFFSET) / 64);
    // Ensure that tcmalloc_slabs partially overlap with
    // __rseq_abi.cpu_id_start as we expect.
    TC_CHECK_EQ(slabs_addr, rseq_abi_addr + TCMALLOC_RSEQ_SLABS_OFFSET);
    // Ensure Sampler is properly aligned.
    TC_CHECK_EQ(sampler_addr % TCMALLOC_SAMPLER_ALIGN, 0);
    // Ensure that tcmalloc_sampler is located before tcmalloc_slabs.
    TC_CHECK_LE(sampler_addr + TCMALLOC_SAMPLER_SIZE, slabs_addr);

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
  // On the first trip through this function do the necessary process-wide
  // initialization work.
  //
  // We do this with all signals disabled so that we don't deadlock due to
  // re-entering from a signal handler.
  //
  // We use a global atomic to record an 'initialized' state as a fast path
  // check, which allows us to avoid the signal mask syscall that we must
  // use to prevent nested initialization during a signal deadlocking on
  // LowLevelOnceInit, before we can enter the 'init once' logic.
  ABSL_CONST_INIT static std::atomic<bool> initialized(false);
  if (!initialized.load(std::memory_order_acquire)) {
    ScopedSigmask mask;

    absl::base_internal::LowLevelCallOnce(&init_per_cpu_once, [&] {
      InitPerCpu();

      // Set `initialized` to true after all initialization has completed.
      // The below store orders with the load acquire further up, i.e., all
      // initialization and side effects thereof are visible to any thread
      // observing a true value in the fast path check.
      initialized.store(true, std::memory_order_release);
    });
  }

  // Once we've decided fast-cpu support is available, initialization for all
  // subsequent threads must succeed for consistency.
  if (init_status == kFastMode && RseqCpuId() == kCpuIdUninitialized) {
    TC_CHECK(InitThreadPerCpu());
  }

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  // If we've decided to use slow mode, set the thread-local CPU ID to
  // __rseq_abi.cpu_id so that IsFast doesn't call this function again for
  // this thread.
  if (init_status == kSlowMode) {
    __rseq_abi.cpu_id = kCpuIdUnsupported;
  }
#endif

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
  TC_CHECK_EQ(errno, EINVAL);
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
  TC_CHECK_EQ(0, sched_getaffinity(0, sizeof(cpu_set_t), &old));

  // Here's the basic idea: if we run on every CPU, then every thread
  // that runs after us has certainly seen every store we've made up
  // to this point, so we pin ourselves to each CPU in turn.
  //
  // But we can't run everywhere; our control plane may have set cpuset.cpus to
  // some subset of CPUs (and may be changing it as we speak.)  On the plus
  // side, if we are unable to run on a particular CPU, the same is true for our
  // siblings (up to some races, dealt with below), so we don't need to.

  for (int cpu = 0, n = NumCPUs(); cpu < n; ++cpu) {
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
  TC_CHECK_GE(fd, 0);

  char c;
  TC_CHECK_EQ(1, signal_safe_read(fd, &c, 1, nullptr));
  TC_CHECK_EQ(0, signal_safe_close(fd));

  // Try to go back to what we originally had before Fence.
  if (0 != sched_setaffinity(0, sizeof(cpu_set_t), &old)) {
    TC_CHECK_EQ(EINVAL, errno);
    // The original set is no longer valid, which should only happen if
    // cpuset.cpus was changed at some point in Fence.  If that happened and we
    // didn't fence, our control plane would have rewritten our affinity mask to
    // everything in cpuset.cpus, so do that.
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0, n = NumCPUs(); i < n; ++i) {
      CPU_SET(i, &set);
    }
    TC_CHECK_EQ(0, sched_setaffinity(0, sizeof(cpu_set_t), &set));
  }
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
static void UpstreamRseqFenceCpu(int cpu) {
  TC_CHECK(using_upstream_fence.load(std::memory_order_relaxed) &&
           "upstream fence unavailable.");

  constexpr int kMEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ = (1 << 7);
  constexpr int kMEMBARRIER_CMD_FLAG_CPU = (1 << 0);

  int64_t res = syscall(__NR_membarrier, kMEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ,
                        kMEMBARRIER_CMD_FLAG_CPU, cpu);

  TC_CHECK(res == 0 || res == -ENXIO /* missing CPU */,
           "Upstream fence failed.");
}
#endif  // TCMALLOC_INTERNAL_PERCPU_USE_RSEQ

// Interrupt every concurrently running sibling thread on "cpu", and guarantee
// our writes up til now are visible to every other CPU. (cpu == -1 is
// equivalent to all CPUs.)
static void FenceInterruptCPU(int cpu) {
  TC_CHECK(IsFast());

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
    return;
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
