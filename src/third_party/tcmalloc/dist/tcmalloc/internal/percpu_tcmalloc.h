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

#ifndef TCMALLOC_INTERNAL_PERCPU_TCMALLOC_H_
#define TCMALLOC_INTERNAL_PERCPU_TCMALLOC_H_

#if defined(__linux__)
#include <linux/param.h>
#else
#include <sys/param.h>
#endif
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/optimization.h"
#include "absl/functional/function_ref.h"
#include "absl/numeric/bits.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/internal/percpu.h"
#include "tcmalloc/internal/prefetch.h"
#include "tcmalloc/internal/sysinfo.h"

#if __clang_major__ >= 11
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT 1
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct PerCPUMetadataState {
  size_t virtual_size;
  size_t resident_size;
};

// The bit denotes that tcmalloc_slabs contains valid slabs offset.
constexpr inline uintptr_t kCachedSlabsBit = 63;

struct ResizeSlabsInfo {
  void* old_slabs;
  size_t old_slabs_size;
};

namespace subtle {
namespace percpu {

enum class Shift : uint8_t;
constexpr uint8_t ToUint8(Shift shift) { return static_cast<uint8_t>(shift); }
constexpr Shift ToShiftType(size_t shift) {
  TC_ASSERT_EQ(ToUint8(static_cast<Shift>(shift)), shift);
  return static_cast<Shift>(shift);
}

// The allocation size for the slabs array.
inline size_t GetSlabsAllocSize(Shift shift, int num_cpus) {
  return static_cast<size_t>(num_cpus) << ToUint8(shift);
}

// Since we lazily initialize our slab, we expect it to be mmap'd and not
// resident.  We align it to a page size so neighboring allocations (from
// TCMalloc's internal arena) do not necessarily cause the metadata to be
// faulted in.
//
// We prefer a small page size (EXEC_PAGESIZE) over the anticipated huge page
// size to allow small-but-slow to allocate the slab in the tail of its
// existing Arena block.
static constexpr std::align_val_t kPhysicalPageAlign{EXEC_PAGESIZE};

// Tcmalloc slab for per-cpu caching mode.
// Conceptually it is equivalent to an array of NumClasses PerCpuSlab's,
// and in fallback implementation it is implemented that way. But optimized
// implementation uses more compact layout and provides faster operations.
//
// Methods of this type must only be used in threads where it is known that the
// percpu primitives are available and percpu::IsFast() has previously returned
// 'true'.
class TcmallocSlab {
 public:
  using DrainHandler = absl::FunctionRef<void(
      int cpu, size_t size_class, void** batch, size_t size, size_t cap)>;
  using ShrinkHandler =
      absl::FunctionRef<void(size_t size_class, void** batch, size_t size)>;

  // We use a single continuous region of memory for all slabs on all CPUs.
  // This region is split into NumCPUs regions of a power-of-2 size
  // (32/64/128/256/512k).
  // First num_classes_ words of each CPU region are occupied by slab
  // headers (Header struct). The remaining memory contain slab arrays.
  // struct Slabs {
  //  std::atomic<int32_t> header[NumClasses];
  //  void* mem[];
  // };

  constexpr TcmallocSlab() = default;

  // Init must be called before any other methods.
  // <slabs> is memory for the slabs with size corresponding to <shift>.
  // <capacity> callback returns max capacity for size class <size_class>.
  // <shift> indicates the number of bits to shift the CPU ID in order to
  //     obtain the location of the per-CPU slab.
  //
  // Initial capacity is 0 for all slabs.
  void Init(size_t num_classes,
            absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
            void* slabs, absl::FunctionRef<size_t(size_t)> capacity,
            Shift shift);

  void InitSlabs(void* slabs, Shift shift,
                 absl::FunctionRef<size_t(size_t)> capacity);

  // Lazily initializes the slab for a specific cpu.
  // <capacity> callback returns max capacity for size class <size_class>.
  //
  // Prior to InitCpu being called on a particular `cpu`, non-const operations
  // other than Push/Pop/PushBatch/PopBatch are invalid.
  void InitCpu(int cpu, absl::FunctionRef<size_t(size_t)> capacity);

  // Grows or shrinks the size of the slabs to use the <new_shift> value. First
  // we initialize <new_slabs>, then lock all headers on the old slabs,
  // atomically update to use the new slabs, and teardown the old slabs. Returns
  // a pointer to old slabs to be madvised away along with the size of the old
  // slabs and the number of bytes that were reused.
  //
  // <alloc> is memory allocation callback (e.g. malloc).
  // <capacity> callback returns max capacity for size class <cl>.
  // <populated> returns whether the corresponding cpu has been populated.
  //
  // Caller must ensure that there are no concurrent calls to InitCpu,
  // ShrinkOtherCache, or Drain.
  ABSL_MUST_USE_RESULT ResizeSlabsInfo ResizeSlabs(
      Shift new_shift, void* new_slabs,
      absl::FunctionRef<size_t(size_t)> capacity,
      absl::FunctionRef<bool(size_t)> populated, DrainHandler drain_handler);

  // For tests. Returns the freed slabs pointer.
  void* Destroy(absl::FunctionRef<void(void*, size_t, std::align_val_t)> free);

  // Number of elements in cpu/size_class slab.
  size_t Length(int cpu, size_t size_class) const;

  // Number of elements (currently) allowed in cpu/size_class slab.
  size_t Capacity(int cpu, size_t size_class) const;

  // If running on cpu, increment the cpu/size_class slab's capacity to no
  // greater than min(capacity+len, max_capacity(<shift>)) and return the
  // increment applied. Otherwise return 0.
  // <max_capacity> is a callback that takes the current slab shift as input and
  // returns the max capacity of <size_class> for that shift value - this is in
  // order to ensure that the shift value used is consistent with the one used
  // in the rest of this function call. Note: max_capacity must be the same as
  // returned by capacity callback passed to Init.
  size_t Grow(int cpu, size_t size_class, size_t len,
              absl::FunctionRef<size_t(uint8_t)> max_capacity);

  // Add an item (which must be non-zero) to the current CPU's slab. Returns
  // true if add succeeds. Otherwise invokes <overflow_handler> and returns
  // false (assuming that <overflow_handler> returns negative value).
  bool Push(size_t size_class, void* item);

  // Remove an item (LIFO) from the current CPU's slab. If the slab is empty,
  // invokes <underflow_handler> and returns its result.
  ABSL_MUST_USE_RESULT void* Pop(size_t class_size);

  // Add up to <len> items to the current cpu slab from the array located at
  // <batch>. Returns the number of items that were added (possibly 0). All
  // items not added will be returned at the start of <batch>. Items are not
  // added if there is no space on the current cpu, or if the thread was
  // re-scheduled since last Push/Pop.
  // REQUIRES: len > 0.
  size_t PushBatch(size_t size_class, void** batch, size_t len);

  // Pop up to <len> items from the current cpu slab and return them in <batch>.
  // Returns the number of items actually removed. If the thread was
  // re-scheduled since last Push/Pop, the function returns 0.
  // REQUIRES: len > 0.
  size_t PopBatch(size_t size_class, void** batch, size_t len);

  // Caches the current cpu slab offset in tcmalloc_slabs if it wasn't
  // cached and the cpu is not stopped. Returns the current cpu and the flag
  // if the offset was previously uncached and is now cached. If the cpu
  // is stopped, returns {-1, true}.
  std::pair<int, bool> CacheCpuSlab();

  // Uncaches the slab offset for the current thread, so that the next Push/Pop
  // operation will return false.
  void UncacheCpuSlab();

  // Synchronization protocol between local and remote operations.
  // This class supports a set of cpu local operations (Push/Pop/
  // PushBatch/PopBatch/Grow), and a set of remote operations that
  // operate on non-current cpu's slab (GrowOtherCache/ShrinkOtherCache/
  // Drain/Resize). Local operations always use a restartable sequence
  // that aborts if the slab pointer (tcamlloc_slab) is uncached.
  // Caching of the slab pointer after rescheduling checks if
  // stopped_[cpu] is unset. Remote operations set stopped_[cpu]
  // and then execute Fence, this ensures that any local operation
  // on the cpu will abort without changing any state and that the
  // slab pointer won't be cached on the cpu. This part uses relaxed atomic
  // operations on stopped_[cpu] because the Fence provides all necessary
  // synchronization between remote and local threads. When a remote operation
  // finishes, it unsets stopped_[cpu] using release memory ordering.
  // This ensures that any new local operation on the cpu that observes
  // unset stopped_[cpu] with acquire memory ordering, will also see all
  // side-effects of the remote operation, and won't interfere with it.
  // StopCpu/StartCpu implement the corresponding parts of the remote
  // synchronization protocol.
  void StopCpu(int cpu);
  void StartCpu(int cpu);

  // Grows the cpu/size_class slab's capacity to no greater than
  // min(capacity+len, max_capacity(<shift>)) and returns the increment
  // applied.
  // <max_capacity> is a callback that takes the current slab shift as input and
  // returns the max capacity of <size_class> for that shift value - this is in
  // order to ensure that the shift value used is consistent with the one used
  // in the rest of this function call. Note: max_capacity must be the same as
  // returned by capacity callback passed to Init.
  // This may be called from another processor, not just the <cpu>.
  size_t GrowOtherCache(int cpu, size_t size_class, size_t len,
                        absl::FunctionRef<size_t(uint8_t)> max_capacity);

  // Decrements the cpu/size_class slab's capacity to no less than
  // max(capacity-len, 0) and returns the actual decrement applied. It attempts
  // to shrink any unused capacity (i.e end-current) in cpu/size_class's slab;
  // if it does not have enough unused items, it pops up to <len> items from
  // cpu/size_class slab and then shrinks the freed capacity.
  //
  // May be called from another processor, not just the <cpu>.
  // REQUIRES: len > 0.
  size_t ShrinkOtherCache(int cpu, size_t size_class, size_t len,
                          ShrinkHandler shrink_handler);

  // Remove all items (of all classes) from <cpu>'s slab; reset capacity for all
  // classes to zero.  Then, for each sizeclass, invoke
  // DrainHandler(size_class, <items from slab>, <previous slab capacity>);
  //
  // It is invalid to concurrently execute Drain() for the same CPU; calling
  // Push/Pop/Grow/Shrink concurrently (even on the same CPU) is safe.
  void Drain(int cpu, DrainHandler drain_handler);

  PerCPUMetadataState MetadataMemoryUsage() const;

  // Gets the current shift of the slabs. Intended for use by the thread that
  // calls ResizeSlabs().
  uint8_t GetShift() const {
    return ToUint8(GetSlabsAndShift(std::memory_order_relaxed).second);
  }

 private:
  // In order to support dynamic slab metadata sizes, we need to be able to
  // atomically update both the slabs pointer and the shift value so we store
  // both together in an atomic SlabsAndShift, which manages the bit operations.
  class SlabsAndShift {
   public:
    // These masks allow for distinguishing the shift bits from the slabs
    // pointer bits. The maximum shift value is less than kShiftMask and
    // kShiftMask is less than kPhysicalPageAlign.
    static constexpr size_t kShiftMask = 0xFF;
    static constexpr size_t kSlabsMask = ~kShiftMask;

    constexpr explicit SlabsAndShift() noexcept : raw_(0) {}
    SlabsAndShift(const void* slabs, Shift shift)
        : raw_(reinterpret_cast<uintptr_t>(slabs) | ToUint8(shift)) {
      TC_ASSERT_EQ(raw_ & kShiftMask, ToUint8(shift));
      TC_ASSERT_EQ(reinterpret_cast<void*>(raw_ & kSlabsMask), slabs);
    }

    std::pair<void*, Shift> Get() const {
      static_assert(kShiftMask >= 0 && kShiftMask <= UCHAR_MAX,
                    "kShiftMask must fit in a uint8_t");
      // Avoid expanding the width of Shift else the compiler will insert an
      // additional instruction to zero out the upper bits on the critical path
      // of alloc / free.  Not zeroing out the bits is safe because both ARM and
      // x86 only use the lowest byte for shift count in variable shifts.
      return {reinterpret_cast<void*>(raw_ & kSlabsMask),
              static_cast<Shift>(raw_ & kShiftMask)};
    }

    bool operator!=(const SlabsAndShift& other) const {
      return raw_ != other.raw_;
    }

   private:
    uintptr_t raw_;
  };

  // Slab header (packed, atomically updated 32-bit).
  // Current and end are pointer offsets from per-CPU region start.
  // The slot array is prefixed with an item that has low bit set and ends
  // at end, and the occupied slots are up to current.
  struct Header {
    // The end offset of the currently occupied slots.
    uint16_t current;
    // The end offset of the slot array for this size class.
    uint16_t end;
  };

  using AtomicHeader = std::atomic<int32_t>;

  // We cast Header to AtomicHeader.
  static_assert(sizeof(Header) == sizeof(AtomicHeader));

  // We mark the pointer that's stored right before size class object range
  // in the slabs array with this mask. When we reach pointer marked with this
  // mask when popping, we understand that we reached the beginning of the
  // range (the slab is empty). The pointer is also a valid pointer for
  // prefetching, so it allows us to always prefetch the previous element
  // when popping.
  static constexpr uintptr_t kBeginMark = 1;

  // It's important that we use consistent values for slabs/shift rather than
  // loading from the atomic repeatedly whenever we use one of the values.
  ABSL_MUST_USE_RESULT std::pair<void*, Shift> GetSlabsAndShift(
      std::memory_order order) const {
    return slabs_and_shift_.load(order).Get();
  }

  static void* CpuMemoryStart(void* slabs, Shift shift, int cpu);
  static AtomicHeader* GetHeader(void* slabs, Shift shift, int cpu,
                                 size_t size_class);
  static Header LoadHeader(AtomicHeader* hdrp);
  static void StoreHeader(AtomicHeader* hdrp, Header hdr);
  static void LockHeader(void* slabs, Shift shift, int cpu, size_t size_class);
  void DrainCpu(void* slabs, Shift shift, int cpu, DrainHandler drain_handler);

  // Implementation of InitCpu() allowing for reuse in ResizeSlabs().
  void InitCpuImpl(void* slabs, Shift shift, int cpu,
                   absl::FunctionRef<size_t(size_t)> capacity);

  std::pair<int, bool> CacheCpuSlabSlow();

  size_t num_classes_ = 0;
  // We store both a pointer to the array of slabs and the shift value together
  // so that we can atomically update both with a single store.
  std::atomic<SlabsAndShift> slabs_and_shift_{};
  // This is in units of bytes.
  size_t virtual_cpu_id_offset_ = offsetof(kernel_rseq, cpu_id);
  // Remote Cpu operation (Resize/Drain/Grow/Shrink) is running so any local
  // operations (Push/Pop) should fail.
  std::atomic<bool>* stopped_ = nullptr;
  // begins_[size_class] is offset of the size_class region in the slabs area.
  std::atomic<uint16_t>* begins_ = nullptr;
};

// RAII for StopCpu/StartCpu.
class ScopedSlabCpuStop {
 public:
  ScopedSlabCpuStop(TcmallocSlab& slab, int cpu) : slab_(slab), cpu_(cpu) {
    slab_.StopCpu(cpu_);
  }

  ~ScopedSlabCpuStop() { slab_.StartCpu(cpu_); }

 private:
  TcmallocSlab& slab_;
  const int cpu_;

  ScopedSlabCpuStop(const ScopedSlabCpuStop&) = delete;
  ScopedSlabCpuStop& operator=(const ScopedSlabCpuStop&) = delete;
};

inline size_t TcmallocSlab::Length(int cpu, size_t size_class) const {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
  uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
  // We can read inconsistent hdr/begin during Resize, to avoid surprising
  // callers return 0 instead of overflows values.
  return std::max<ssize_t>(0, hdr.current - begin);
}

inline size_t TcmallocSlab::Capacity(int cpu, size_t size_class) const {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
  uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
  return std::max<ssize_t>(0, hdr.end - begin);
}

#if defined(__x86_64__)
#define TCMALLOC_RSEQ_RELOC_TYPE "R_X86_64_NONE"
#define TCMALLOC_RSEQ_JUMP "jmp"
#if !defined(__PIC__) && !defined(__PIE__)
#define TCMALLOC_RSEQ_SET_CS(name) \
  "movq $__rseq_cs_" #name "_%=, %[rseq_cs_addr]\n"
#else
#define TCMALLOC_RSEQ_SET_CS(name) \
  "lea __rseq_cs_" #name           \
  "_%=(%%rip), %[scratch]\n"       \
  "movq %[scratch], %[rseq_cs_addr]\n"
#endif

#elif defined(__aarch64__)
// The trampoline uses a non-local branch to restart critical sections.
// The trampoline is located in the .text.unlikely section, and the maximum
// distance of B and BL branches in ARM64 is limited to 128MB. If the linker
// detects the distance being too large, it injects a thunk which may clobber
// the x16 or x17 register according to the ARMv8 ABI standard.
// The actual clobbering is hard to trigger in a test, so instead of waiting
// for clobbering to happen in production binaries, we proactively always
// clobber x16 and x17 to shake out bugs earlier.
// RSEQ critical section asm blocks should use TCMALLOC_RSEQ_CLOBBER
// in the clobber list to account for this.
#ifndef NDEBUG
#define TCMALLOC_RSEQ_TRAMPLINE_SMASH \
  "mov x16, #-2097\n"                 \
  "mov x17, #-2099\n"
#else
#define TCMALLOC_RSEQ_TRAMPLINE_SMASH
#endif
#define TCMALLOC_RSEQ_CLOBBER "x16", "x17"
#define TCMALLOC_RSEQ_RELOC_TYPE "R_AARCH64_NONE"
#define TCMALLOC_RSEQ_JUMP "b"
#define TCMALLOC_RSEQ_SET_CS(name)                     \
  TCMALLOC_RSEQ_TRAMPLINE_SMASH                        \
  "adrp %[scratch], __rseq_cs_" #name                  \
  "_%=\n"                                              \
  "add %[scratch], %[scratch], :lo12:__rseq_cs_" #name \
  "_%=\n"                                              \
  "str %[scratch], %[rseq_cs_addr]\n"
#endif

#if !defined(__clang_major__) || __clang_major__ >= 9
#define TCMALLOC_RSEQ_RELOC ".reloc 0, " TCMALLOC_RSEQ_RELOC_TYPE ", 1f\n"
#else
#define TCMALLOC_RSEQ_RELOC
#endif

// Common rseq asm prologue.
// It uses labels 1-4 and assumes the critical section ends with label 5.
// The prologue assumes there is [scratch] input with a scratch register.
#define TCMALLOC_RSEQ_PROLOGUE(name)                                          \
  /* __rseq_cs only needs to be writeable to allow for relocations.*/         \
  ".pushsection __rseq_cs, \"aw?\"\n"                                         \
  ".balign 32\n"                                                              \
  ".local __rseq_cs_" #name                                                   \
  "_%=\n"                                                                     \
  ".type __rseq_cs_" #name                                                    \
  "_%=,@object\n"                                                             \
  ".size __rseq_cs_" #name                                                    \
  "_%=,32\n"                                                                  \
  "__rseq_cs_" #name                                                          \
  "_%=:\n"                                                                    \
  ".long 0x0\n"                                                               \
  ".long 0x0\n"                                                               \
  ".quad 4f\n"                                                                \
  ".quad 5f - 4f\n"                                                           \
  ".quad 2f\n"                                                                \
  ".popsection\n" TCMALLOC_RSEQ_RELOC                                         \
  ".pushsection __rseq_cs_ptr_array, \"aw?\"\n"                               \
  "1:\n"                                                                      \
  ".balign 8\n"                                                               \
  ".quad __rseq_cs_" #name                                                    \
  "_%=\n" /* Force this section to be retained.                               \
             It is for debugging, but is otherwise not referenced. */         \
  ".popsection\n"                                                             \
  ".pushsection .text.unlikely, \"ax?\"\n" /* This is part of the upstream    \
                                              rseq ABI.  The 4 bytes prior to \
                                              the abort IP must match         \
                                              TCMALLOC_PERCPU_RSEQ_SIGNATURE  \
                                              (as configured by our rseq      \
                                              syscall's signature parameter). \
                                              This signature is used to       \
                                              annotate valid abort IPs (since \
                                              rseq_cs could live in a         \
                                              user-writable segment). */      \
  ".long %c[rseq_sig]\n"                                                      \
  ".local " #name                                                             \
  "_trampoline_%=\n"                                                          \
  ".type " #name                                                              \
  "_trampoline_%=,@function\n"                                                \
  "" #name                                                                    \
  "_trampoline_%=:\n"                                                         \
  "2:\n" TCMALLOC_RSEQ_JUMP                                                   \
  " 3f\n"                                                                     \
  ".size " #name "_trampoline_%=, . - " #name                                 \
  "_trampoline_%=\n"                                                          \
  ".popsection\n"                   /* Prepare */                             \
  "3:\n" TCMALLOC_RSEQ_SET_CS(name) /* Start */                               \
      "4:\n"

#define TCMALLOC_RSEQ_INPUTS                                                 \
  [rseq_cs_addr] "m"(__rseq_abi.rseq_cs),                                    \
      [rseq_slabs_addr] "m"(*reinterpret_cast<volatile char*>(               \
          reinterpret_cast<uintptr_t>(&__rseq_abi) +                         \
          TCMALLOC_RSEQ_SLABS_OFFSET)),                                      \
      [rseq_sig] "n"(                                                        \
          TCMALLOC_PERCPU_RSEQ_SIGNATURE), /* Also pass common consts, there \
                                              is no cost to passing unused   \
                                              consts. */                     \
      [cached_slabs_bit] "n"(TCMALLOC_CACHED_SLABS_BIT),                     \
      [cached_slabs_mask_neg] "n"(~TCMALLOC_CACHED_SLABS_MASK)

// Store v to p (*p = v) if the current thread wasn't rescheduled
// (still has the slab pointer cached). Otherwise returns false.
template <typename T>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool StoreCurrentCpu(volatile void* p,
                                                         T v) {
  uintptr_t scratch = 0;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__x86_64__)
  asm(TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_StoreCurrentCpu)
          R"(
      xorq %[scratch], %[scratch]
      btq $%c[cached_slabs_bit], %[rseq_slabs_addr]
      jnc 5f
      movl $1, %k[scratch]
      mov %[v], %[p]
      5 :)"
      : [scratch] "=&r"(scratch)
      : TCMALLOC_RSEQ_INPUTS, [p] "m"(*static_cast<void* volatile*>(p)),
        [v] "r"(v)
      : "cc", "memory");
#elif TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
  uintptr_t tmp;
  // Aarch64 requires different argument references for different sizes
  // for the STR instruction (%[v] vs %w[v]), so we have to duplicate
  // the asm block.
  if constexpr (sizeof(T) == sizeof(uint64_t)) {
    asm(TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_StoreCurrentCpu)
            R"(
        mov %[scratch], #0
        ldr %[tmp], %[rseq_slabs_addr]
        tbz %[tmp], #%c[cached_slabs_bit], 5f
        mov %[scratch], #1
        str %[v], %[p]
        5 :)"
        : [scratch] "=&r"(scratch), [tmp] "=&r"(tmp)
        : TCMALLOC_RSEQ_INPUTS, [p] "m"(*static_cast<uint64_t* volatile*>(p)),
          [v] "r"(v)
        : TCMALLOC_RSEQ_CLOBBER, "cc", "memory");
  } else {
    static_assert(sizeof(T) == sizeof(uint32_t));
    asm(TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_StoreCurrentCpu)
            R"(
        mov %[scratch], #0
        ldr %[tmp], %[rseq_slabs_addr]
        tbz %[tmp], #%c[cached_slabs_bit], 5f
        mov %[scratch], #1
        str %w[v], %[p]
        5 :)"
        : [scratch] "=&r"(scratch), [tmp] "=&r"(tmp)
        : TCMALLOC_RSEQ_INPUTS, [p] "m"(*static_cast<uint32_t* volatile*>(p)),
          [v] "r"(v)
        : TCMALLOC_RSEQ_CLOBBER, "cc", "memory");
  }
#endif
  return scratch;
}

// Prefetch slabs memory for the case of repeated pushes/pops.
// Note: this prefetch slows down micro-benchmarks, but provides ~0.1-0.5%
// speedup for larger real applications.
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void PrefetchSlabMemory(uintptr_t ptr) {
  PrefetchWT0(reinterpret_cast<void*>(ptr));
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__x86_64__)
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab_Internal_Push(
    size_t size_class, void* item) {
  uintptr_t scratch, current;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto(
#else
  bool overflow;
  asm volatile(
#endif
      TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Push)
      // scratch = tcmalloc_slabs;
      "movq %[rseq_slabs_addr], %[scratch]\n"
      // if (scratch & TCMALLOC_CACHED_SLABS_MASK>) goto overflow_label;
      // scratch &= ~TCMALLOC_CACHED_SLABS_MASK;
      "btrq $%c[cached_slabs_bit], %[scratch]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "jnc %l[overflow_label]\n"
#else
      "jae 5f\n"  // ae==c
#endif
      // current = slabs->current;
      "movzwq (%[scratch], %[size_class], 4), %[current]\n"
      // if (ABSL_PREDICT_FALSE(current >= slabs->end)) { goto overflow_label; }
      "cmp 2(%[scratch], %[size_class], 4), %w[current]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "jae %l[overflow_label]\n"
#else
      "jae 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccae)
  // If so, the above code needs to explicitly set a ccae return value.
#endif
      "mov %[item], (%[scratch], %[current], 8)\n"
      "lea 1(%[current]), %[current]\n"
      "mov %w[current], (%[scratch], %[size_class], 4)\n"
      // Commit
      "5:\n"
      :
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      [overflow] "=@ccae"(overflow),
#endif
      [scratch] "=&r"(scratch), [current] "=&r"(current)
      : TCMALLOC_RSEQ_INPUTS, [size_class] "r"(size_class), [item] "r"(item)
      : "cc", "memory"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      : overflow_label
#endif
  );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(overflow)) {
    goto overflow_label;
  }
#endif
  // Current now points to the slot we are going to push to next.
  PrefetchSlabMemory(scratch + current * sizeof(void*));
  return true;
overflow_label:
  return false;
}
#endif  // defined(__x86_64__)

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab_Internal_Push(
    size_t size_class, void* item) {
  uintptr_t region_start, scratch, end_ptr, end;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto(
#else
  bool overflow;
  asm volatile(
#endif
      TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Push)
      // region_start = tcmalloc_slabs;
      "ldr %[region_start], %[rseq_slabs_addr]\n"
  // if (region_start & TCMALLOC_CACHED_SLABS_MASK) goto overflow_label;
  // region_start &= ~TCMALLOC_CACHED_SLABS_MASK;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "tbz %[region_start], #%c[cached_slabs_bit], %l[overflow_label]\n"
      "and %[region_start], %[region_start], #%c[cached_slabs_mask_neg]\n"
#else
      "subs %[region_start], %[region_start], %[cached_slabs_mask]\n"
      "b.ls 5f\n"
#endif
      // end_ptr = &(slab_headers[0]->end)
      "add %[end_ptr], %[region_start], #2\n"
      // scratch = slab_headers[size_class]->current (current index)
      "ldrh %w[scratch], [%[region_start], %[size_class_lsl2]]\n"
      // end = slab_headers[size_class]->end (end index)
      "ldrh %w[end], [%[end_ptr], %[size_class_lsl2]]\n"
      // if (ABSL_PREDICT_FALSE(end <= scratch)) { goto overflow_label; }
      "cmp %[end], %[scratch]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "b.ls %l[overflow_label]\n"
#else
      "b.ls 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccls)
  // If so, the above code needs to explicitly set a ccls return value.
#endif
      "str %[item], [%[region_start], %[scratch], LSL #3]\n"
      "add %w[scratch], %w[scratch], #1\n"
      "strh %w[scratch], [%[region_start], %[size_class_lsl2]]\n"
      // Commit
      "5:\n"
      : [end_ptr] "=&r"(end_ptr), [scratch] "=&r"(scratch), [end] "=&r"(end),
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
        [overflow] "=@ccls"(overflow),
#endif
        [region_start] "=&r"(region_start)
      : TCMALLOC_RSEQ_INPUTS,
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
        [cached_slabs_mask] "r"(TCMALLOC_CACHED_SLABS_MASK),
#endif
        [size_class_lsl2] "r"(size_class << 2), [item] "r"(item)
      : TCMALLOC_RSEQ_CLOBBER, "memory"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      , "cc"
      : overflow_label
#endif
  );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(overflow)) {
    goto overflow_label;
  }
#endif
  return true;
overflow_label:
  return false;
}
#endif  // defined (__aarch64__)

inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab::Push(size_t size_class,
                                                            void* item) {
  TC_ASSERT_NE(size_class, 0);
  TC_ASSERT_NE(item, nullptr);
  TC_ASSERT_EQ(reinterpret_cast<uintptr_t>(item) & kBeginMark, 0);
  // Speculatively annotate item as released to TSan.  We may not succeed in
  // pushing the item, but if we wait for the restartable sequence to succeed,
  // it may become visible to another thread before we can trigger the
  // annotation.
  TSANRelease(item);
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  return TcmallocSlab_Internal_Push(size_class, item);
#else
  return false;
#endif
}

// PrefetchNextObject provides a common code path across architectures for
// generating a prefetch of the next object.
//
// It is in a distinct, always-lined method to make its cost more transparent
// when profiling with debug information.
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void PrefetchNextObject(
    void* prefetch_target) {
  // A note about prefetcht0 in Pop:  While this prefetch may appear costly,
  // trace analysis shows the target is frequently used (b/70294962). Stalling
  // on a TLB miss at the prefetch site (which has no deps) and prefetching the
  // line async is better than stalling at the use (which may have deps) to fill
  // the TLB and the cache miss.
  //
  // See "Beyond malloc efficiency to fleet efficiency"
  // (https://research.google/pubs/pub50370/), section 6.4 for additional
  // details.
  //
  // TODO(b/214608320): Evaluate prefetch for write.
  __builtin_prefetch(prefetch_target, 0, 3);
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__x86_64__)
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab::Pop(size_t size_class) {
  TC_ASSERT_NE(size_class, 0);
  void* next;
  void* result;
  uintptr_t scratch, current;

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto(
#else
  bool underflow;
  asm(
#endif
      TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Pop)
      // scratch = tcmalloc_slabs;
      "movq %[rseq_slabs_addr], %[scratch]\n"
      // if (scratch & TCMALLOC_CACHED_SLABS_MASK) goto overflow_label;
      // scratch &= ~TCMALLOC_CACHED_SLABS_MASK;
      "btrq $%c[cached_slabs_bit], %[scratch]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "jnc %l[underflow_path]\n"
#else
      "cmc\n"
      "jc 5f\n"
#endif
      // current = scratch->header[size_class].current;
      "movzwq (%[scratch], %[size_class], 4), %[current]\n"
      "movq -8(%[scratch], %[current], 8), %[result]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "testb $%c[begin_mark_mask], %b[result]\n"
      "jnz %l[underflow_path]\n"
#else
      "btq $%c[begin_mark_bit], %[result]\n"
      "jc 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccc)
  // If so, the above code needs to explicitly set a ccc return value.
#endif
      "movq -16(%[scratch], %[current], 8), %[next]\n"
      "lea -1(%[current]), %[current]\n"
      "movw %w[current], (%[scratch], %[size_class], 4)\n"
      // Commit
      "5:\n"
      : [result] "=&r"(result),
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
        [underflow] "=@ccc"(underflow),
#endif
        [scratch] "=&r"(scratch), [current] "=&r"(current), [next] "=&r"(next)
      : TCMALLOC_RSEQ_INPUTS,
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
        [begin_mark_mask] "n"(kBeginMark),
#else
        [begin_mark_bit] "n"(absl::countr_zero(kBeginMark)),
#endif
        [size_class] "r"(size_class)
      : "cc", "memory"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      : underflow_path
#endif
  );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(underflow)) {
    goto underflow_path;
  }
#endif
  TC_ASSERT(next);
  TC_ASSERT(result);
  TSANAcquire(result);

  // The next pop will be from current-1, but because we prefetch the previous
  // element we've already just read that, so prefetch current-2.
  PrefetchSlabMemory(scratch + (current - 2) * sizeof(void*));
  PrefetchNextObject(next);
  return AssumeNotNull(result);
underflow_path:
  return nullptr;
}
#endif  // defined(__x86_64__)

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab::Pop(size_t size_class) {
  TC_ASSERT_NE(size_class, 0);
  void* result;
  void* region_start;
  void* prefetch;
  uintptr_t scratch;
  uintptr_t previous;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto(
#else
  bool underflow;
  asm(
#endif
      TCMALLOC_RSEQ_PROLOGUE(TcmallocSlab_Internal_Pop)
      // region_start = tcmalloc_slabs;
      "ldr %[region_start], %[rseq_slabs_addr]\n"
  // if (region_start & TCMALLOC_CACHED_SLABS_MASK) goto overflow_label;
  // region_start &= ~TCMALLOC_CACHED_SLABS_MASK;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "tbz %[region_start], #%c[cached_slabs_bit], %l[underflow_path]\n"
#else
      "tst %[region_start], %[cached_slabs_mask]\n"
      "b.eq 5f\n"
#endif
      "and %[region_start], %[region_start], #%c[cached_slabs_mask_neg]\n"
      // scratch = slab_headers[size_class]->current (current index)
      "ldrh %w[scratch], [%[region_start], %[size_class_lsl2]]\n"
      // scratch--
      "sub %w[scratch], %w[scratch], #1\n"
      "ldr %[result], [%[region_start], %[scratch], LSL #3]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      "tbnz %[result], #%c[begin_mark_bit], %l[underflow_path]\n"
#else
      // Temporary use %[previous] to store %[result] with inverted mark bit.
      "eor %[previous], %[result], #%c[begin_mark_mask]\n"
      "tst %[previous], #%c[begin_mark_mask]\n"
      "b.eq 5f\n"
  // Important! code below this must not affect any flags (i.e.: cceq)
  // If so, the above code needs to explicitly set a cceq return value.
#endif
      "sub %w[previous], %w[scratch], #1\n"
      "ldr %[prefetch], [%[region_start], %[previous], LSL #3]\n"
      "strh %w[scratch], [%[region_start], %[size_class_lsl2]]\n"
      // Commit
      "5:\n"
      :
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      [underflow] "=@cceq"(underflow),
#endif
      [result] "=&r"(result), [prefetch] "=&r"(prefetch),
      // Temps
      [region_start] "=&r"(region_start), [previous] "=&r"(previous),
      [scratch] "=&r"(scratch)
      // Real inputs
      : TCMALLOC_RSEQ_INPUTS,
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
        [begin_mark_bit] "n"(absl::countr_zero(kBeginMark)),
#else
        [cached_slabs_mask] "r"(TCMALLOC_CACHED_SLABS_MASK),
#endif
        [begin_mark_mask] "n"(kBeginMark), [size_class] "r"(size_class),
        [size_class_lsl2] "r"(size_class << 2)
      : TCMALLOC_RSEQ_CLOBBER, "memory"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
      , "cc"
      : underflow_path
#endif
  );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(underflow)) {
    goto underflow_path;
  }
#endif
  TSANAcquire(result);
  PrefetchNextObject(prefetch);
  return AssumeNotNull(result);
underflow_path:
  return nullptr;
}
#endif  // defined(__aarch64__)

#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab::Pop(size_t size_class) {
  return nullptr;
}
#endif

inline size_t TcmallocSlab::Grow(
    int cpu, size_t size_class, size_t len,
    absl::FunctionRef<size_t(uint8_t)> max_capacity) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t max_cap = max_capacity(ToUint8(shift));
  auto* hdrp = GetHeader(slabs, shift, cpu, size_class);
  Header hdr = LoadHeader(hdrp);
  uint16_t begin = begins_[size_class].load(std::memory_order_relaxed);
  ssize_t have = static_cast<ssize_t>(max_cap - (hdr.end - begin));
  if (have <= 0) {
    return 0;
  }
  uint16_t n = std::min<uint16_t>(len, have);
  hdr.end += n;
  return StoreCurrentCpu(hdrp, hdr) ? n : 0;
}

inline std::pair<int, bool> TcmallocSlab::CacheCpuSlab() {
  int cpu = GetCurrentVirtualCpuUnsafe(virtual_cpu_id_offset_);
  TC_ASSERT_GE(cpu, 0);
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  if (ABSL_PREDICT_FALSE((tcmalloc_slabs & TCMALLOC_CACHED_SLABS_MASK) == 0)) {
    return CacheCpuSlabSlow();
  }
  // We already have slab offset cached, so the slab is indeed full/empty.
#endif
  return {cpu, false};
}

inline void TcmallocSlab::UncacheCpuSlab() {
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  tcmalloc_slabs = 0;
#endif
}

inline size_t TcmallocSlab::PushBatch(size_t size_class, void** batch,
                                      size_t len) {
  TC_ASSERT_NE(size_class, 0);
  TC_ASSERT_NE(len, 0);
  // We need to annotate batch[...] as released before running the restartable
  // sequence, since those objects become visible to other threads the moment
  // the restartable sequence is complete and before the annotation potentially
  // runs.
  //
  // This oversynchronizes slightly, since PushBatch may succeed only partially.
  TSANReleaseBatch(batch, len);
  return TcmallocSlab_Internal_PushBatch(size_class, batch, len);
}

inline size_t TcmallocSlab::PopBatch(size_t size_class, void** batch,
                                     size_t len) {
  TC_ASSERT_NE(size_class, 0);
  TC_ASSERT_NE(len, 0);
  const size_t n = TcmallocSlab_Internal_PopBatch(size_class, batch, len,
                                                  &begins_[size_class]);
  TC_ASSERT_LE(n, len);

  // PopBatch is implemented in assembly, msan does not know that the returned
  // batch is initialized.
  ANNOTATE_MEMORY_IS_INITIALIZED(batch, n * sizeof(batch[0]));
  TSANAcquireBatch(batch, n);
  return n;
}

inline void* TcmallocSlab::CpuMemoryStart(void* slabs, Shift shift, int cpu) {
  return &static_cast<char*>(slabs)[cpu << ToUint8(shift)];
}

inline auto TcmallocSlab::GetHeader(void* slabs, Shift shift, int cpu,
                                    size_t size_class) -> AtomicHeader* {
  TC_ASSERT_NE(size_class, 0);
  return &static_cast<AtomicHeader*>(
      CpuMemoryStart(slabs, shift, cpu))[size_class];
}

inline auto TcmallocSlab::LoadHeader(AtomicHeader* hdrp) -> Header {
  return absl::bit_cast<Header>(hdrp->load(std::memory_order_relaxed));
}

inline void TcmallocSlab::StoreHeader(AtomicHeader* hdrp, Header hdr) {
  hdrp->store(absl::bit_cast<int32_t>(hdr), std::memory_order_relaxed);
}

}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_PERCPU_TCMALLOC_H_
