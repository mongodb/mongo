// Copyright 2015, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "jit/arm64/vixl/Cpu-vixl.h"
#include "jit/arm64/vixl/Simulator-vixl.h"
#include "jit/arm64/vixl/Utils-vixl.h"
#include "util/WindowsWrapper.h"

#if defined(XP_DARWIN)
#  include <libkern/OSCacheControl.h>
#endif

namespace vixl {

// Currently computes I and D cache line size.
void CPU::SetUp() {
  uint32_t cache_type_register = GetCacheType();

  // The cache type register holds information about the caches, including I
  // D caches line size.
  static const int kDCacheLineSizeShift = 16;
  static const int kICacheLineSizeShift = 0;
  static const uint32_t kDCacheLineSizeMask = 0xf << kDCacheLineSizeShift;
  static const uint32_t kICacheLineSizeMask = 0xf << kICacheLineSizeShift;

  // The cache type register holds the size of the I and D caches in words as
  // a power of two.
  uint32_t dcache_line_size_power_of_two =
      (cache_type_register & kDCacheLineSizeMask) >> kDCacheLineSizeShift;
  uint32_t icache_line_size_power_of_two =
      (cache_type_register & kICacheLineSizeMask) >> kICacheLineSizeShift;

  dcache_line_size_ = 4 << dcache_line_size_power_of_two;
  icache_line_size_ = 4 << icache_line_size_power_of_two;

  // Bug 1521158 suggests that having CPU with different cache line sizes could
  // cause issues as we would only invalidate half of the cache line of we
  // invalidate every 128 bytes, but other little cores have a different stride
  // such as 64 bytes. To be conservative, we will try reducing the stride to 32
  // bytes, which should be smaller than any known cache line.
  const uint32_t conservative_line_size = 32;
  dcache_line_size_ = std::min(dcache_line_size_, conservative_line_size);
  icache_line_size_ = std::min(icache_line_size_, conservative_line_size);
}


uint32_t CPU::GetCacheType() {
#if defined(__aarch64__) && (defined(__linux__) || defined(__android__))
  uint64_t cache_type_register;
  // Copy the content of the cache type register to a core register.
  __asm__ __volatile__ ("mrs %[ctr], ctr_el0"  // NOLINT
                        : [ctr] "=r" (cache_type_register));
  VIXL_ASSERT(IsUint32(cache_type_register));
  return static_cast<uint32_t>(cache_type_register);
#else
  // This will lead to a cache with 1 byte long lines, which is fine since
  // neither EnsureIAndDCacheCoherency nor the simulator will need this
  // information.
  return 0;
#endif
}

void CPU::EnsureIAndDCacheCoherency(void* address, size_t length) {
#if defined(JS_SIMULATOR_ARM64) && defined(JS_CACHE_SIMULATOR_ARM64)
  // This code attempts to emulate what the following assembly sequence is
  // doing, which is sending the information to all cores that some cache line
  // have to be invalidated and invalidating them only on the current core.
  //
  // This is done by recording the current range to be flushed to all
  // simulators, then if there is a simulator associated with the current
  // thread, applying all flushed ranges as the "isb" instruction would do.
  //
  // As we have no control over the CPU cores used by the code generator and the
  // execution threads, this code assumes that each thread runs on its own core.
  //
  // See Bug 1529933 for more detailed explanation of this issue.
  using js::jit::SimulatorProcess;
  js::jit::AutoLockSimulatorCache alsc;
  if (length > 0) {
    SimulatorProcess::recordICacheFlush(address, length);
  }
  Simulator* sim = vixl::Simulator::Current();
  if (sim) {
    sim->FlushICache();
  }
#elif defined(_MSC_VER) && defined(_M_ARM64)
  FlushInstructionCache(GetCurrentProcess(), address, length);
#elif defined(XP_DARWIN)
  sys_icache_invalidate(address, length);
#elif defined(__aarch64__) && (defined(__linux__) || defined(__android__))
  // Implement the cache synchronisation for all targets where AArch64 is the
  // host, even if we're building the simulator for an AAarch64 host. This
  // allows for cases where the user wants to simulate code as well as run it
  // natively.

  if (length == 0) {
    return;
  }

  // The code below assumes user space cache operations are allowed.

  // Work out the line sizes for each cache, and use them to determine the
  // start addresses.
  uintptr_t start = reinterpret_cast<uintptr_t>(address);
  uintptr_t dsize = static_cast<uintptr_t>(dcache_line_size_);
  uintptr_t isize = static_cast<uintptr_t>(icache_line_size_);
  uintptr_t dline = start & ~(dsize - 1);
  uintptr_t iline = start & ~(isize - 1);

  // Cache line sizes are always a power of 2.
  VIXL_ASSERT(IsPowerOf2(dsize));
  VIXL_ASSERT(IsPowerOf2(isize));
  uintptr_t end = start + length;

  do {
    __asm__ __volatile__ (
      // Clean each line of the D cache containing the target data.
      //
      // dc       : Data Cache maintenance
      //     c    : Clean
      //      i   : Invalidate
      //      va  : by (Virtual) Address
      //        c : to the point of Coherency
      // Original implementation used cvau, but changed to civac due to
      // errata on Cortex-A53 819472, 826319, 827319 and 824069.
      // See ARM DDI 0406B page B2-12 for more information.
      //
      "   dc    civac, %[dline]\n"
      :
      : [dline] "r" (dline)
      // This code does not write to memory, but the "memory" dependency
      // prevents GCC from reordering the code.
      : "memory");
    dline += dsize;
  } while (dline < end);

  __asm__ __volatile__ (
    // Make sure that the data cache operations (above) complete before the
    // instruction cache operations (below).
    //
    // dsb      : Data Synchronisation Barrier
    //      ish : Inner SHareable domain
    //
    // The point of unification for an Inner Shareable shareability domain is
    // the point by which the instruction and data caches of all the processors
    // in that Inner Shareable shareability domain are guaranteed to see the
    // same copy of a memory location.  See ARM DDI 0406B page B2-12 for more
    // information.
    "   dsb   ish\n"
    : : : "memory");

  do {
    __asm__ __volatile__ (
      // Invalidate each line of the I cache containing the target data.
      //
      // ic      : Instruction Cache maintenance
      //    i    : Invalidate
      //     va  : by Address
      //       u : to the point of Unification
      "   ic   ivau, %[iline]\n"
      :
      : [iline] "r" (iline)
      : "memory");
    iline += isize;
  } while (iline < end);

  __asm__ __volatile__(
      // Make sure that the instruction cache operations (above) take effect
      // before the isb (below).
      "   dsb  ish\n"

      // Ensure that any instructions already in the pipeline are discarded and
      // reloaded from the new data.
      // isb : Instruction Synchronisation Barrier
      "   isb\n"
      :
      :
      : "memory");
#else
  // If the host isn't AArch64, we must be using the simulator, so this function
  // doesn't have to do anything.
  USE(address, length);
#endif
}

void CPU::FlushExecutionContext() {
#if defined(JS_SIMULATOR_ARM64) && defined(JS_CACHE_SIMULATOR_ARM64)
  // Performing an 'isb' will ensure the current core instruction pipeline is
  // synchronized with an icache flush executed by another core.
  using js::jit::SimulatorProcess;
  js::jit::AutoLockSimulatorCache alsc;
  Simulator* sim = vixl::Simulator::Current();
  if (sim) {
    sim->FlushICache();
  }
#elif defined(__aarch64__)
  // Ensure that any instructions already in the pipeline are discarded and
  // reloaded from the icache.
  __asm__ __volatile__("isb\n" : : : "memory");
#endif
}

}  // namespace vixl
