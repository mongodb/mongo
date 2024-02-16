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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/casts.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/functional/function_ref.h"
#include "tcmalloc/internal/mincore.h"
#include "tcmalloc/internal/percpu.h"

#if defined(TCMALLOC_INTERNAL_PERCPU_USE_RSEQ)
#if !defined(__clang__)
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO 1
#elif __clang_major__ >= 9 && !__has_feature(speculative_load_hardening)
// asm goto requires the use of Clang 9 or newer:
// https://releases.llvm.org/9.0.0/tools/clang/docs/ReleaseNotes.html#c-language-changes-in-clang
//
// SLH (Speculative Load Hardening) builds do not support asm goto.  We can
// detect these compilation modes since
// https://github.com/llvm/llvm-project/commit/379e68a763097bed55556c6dc7453e4b732e3d68.
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO 1
#if __clang_major__ >= 11
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT 1
#endif

#else
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO 0
#endif
#else
#define TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO 0
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

struct PerCPUMetadataState {
  size_t virtual_size;
  size_t resident_size;
};

// Determine number of bits we should use for allocating per-cpu cache.
// The amount of per-cpu cache is 2 ^ per-cpu-shift.
// When dynamic slab size is enabled, we start with kInitialPerCpuShift and
// grow as needed up to kMaxPerCpuShift. When dynamic slab size is disabled,
// we always use kMaxPerCpuShift.
#if defined(TCMALLOC_SMALL_BUT_SLOW)
constexpr inline uint8_t kInitialPerCpuShift = 12;
constexpr inline uint8_t kMaxPerCpuShift = 12;
#else
constexpr inline uint8_t kInitialPerCpuShift = 14;
constexpr inline uint8_t kMaxPerCpuShift = 18;
#endif

constexpr inline uint8_t kNumPossiblePerCpuShifts =
    kMaxPerCpuShift - kInitialPerCpuShift + 1;

struct ResizeSlabsInfo {
  void* old_slabs;
  size_t old_slabs_size;
};

namespace subtle {
namespace percpu {

enum class Shift : uint8_t;
constexpr uint8_t ToUint8(Shift shift) { return static_cast<uint8_t>(shift); }
constexpr Shift ToShiftType(size_t shift) {
  ASSERT(ToUint8(static_cast<Shift>(shift)) == shift);
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
template <size_t NumClasses>
class TcmallocSlab {
 public:
  using DrainHandler = absl::FunctionRef<void(
      int cpu, size_t size_class, void** batch, size_t size, size_t cap)>;
  using ShrinkHandler =
      absl::FunctionRef<void(size_t size_class, void** batch, size_t size)>;

  // We use a single continuous region of memory for all slabs on all CPUs.
  // This region is split into NumCPUs regions of size kPerCpuMem (256k).
  // First NumClasses words of each CPU region are occupied by slab
  // headers (Header struct). The remaining memory contain slab arrays.
  struct Slabs {
    std::atomic<int64_t> header[NumClasses];
    void* mem[];
  };

  constexpr TcmallocSlab() = default;

  // Init must be called before any other methods.
  // <slabs> is memory for the slabs with size corresponding to <shift>.
  // <capacity> callback returns max capacity for size class <size_class>.
  // <shift> indicates the number of bits to shift the CPU ID in order to
  //     obtain the location of the per-CPU slab.
  //
  // Initial capacity is 0 for all slabs.
  void Init(Slabs* slabs, absl::FunctionRef<size_t(size_t)> capacity,
            Shift shift);

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
      Shift new_shift, Slabs* new_slabs,
      absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
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

  // If running on cpu, decrement the cpu/size_class slab's capacity to no less
  // than max(capacity-len, 0) and return the actual decrement applied.
  // Otherwise return 0.
  size_t Shrink(int cpu, size_t size_class, size_t len);

  // Add an item (which must be non-zero) to the current CPU's slab. Returns
  // true if add succeeds. Otherwise invokes <overflow_handler> and returns
  // false (assuming that <overflow_handler> returns negative value).
  bool Push(size_t size_class, void* item, OverflowHandler overflow_handler,
            void* arg);

  // Remove an item (LIFO) from the current CPU's slab. If the slab is empty,
  // invokes <underflow_handler> and returns its result.
  ABSL_MUST_USE_RESULT void* Pop(size_t size_class,
                                 UnderflowHandler underflow_handler, void* arg);

  // Add up to <len> items to the current cpu slab from the array located at
  // <batch>. Returns the number of items that were added (possibly 0). All
  // items not added will be returned at the start of <batch>. Items are only
  // not added if there is no space on the current cpu.
  // REQUIRES: len > 0.
  size_t PushBatch(size_t size_class, void** batch, size_t len);

  // Pop up to <len> items from the current cpu slab and return them in <batch>.
  // Returns the number of items actually removed.
  // REQUIRES: len > 0.
  size_t PopBatch(size_t size_class, void** batch, size_t len);

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

  inline int GetCurrentVirtualCpuUnsafe() {
    return VirtualRseqCpuId(virtual_cpu_id_offset_);
  }

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
    // These masks allow for distinguishing the shift bits from the slabs
    // pointer bits. The maximum shift value is less than kShiftMask and
    // kShiftMask is less than kPhysicalPageAlign.
    static constexpr size_t kShiftMask = 0xFF;
    static constexpr size_t kSlabsMask = ~kShiftMask;

   public:
    constexpr explicit SlabsAndShift() noexcept : raw_(0) {}
    SlabsAndShift(const Slabs* slabs, Shift shift)
        : raw_(reinterpret_cast<uintptr_t>(slabs) | ToUint8(shift)) {
      ASSERT((raw_ & kShiftMask) == ToUint8(shift));
      ASSERT(reinterpret_cast<Slabs*>(raw_ & kSlabsMask) == slabs);
    }

    std::pair<Slabs*, Shift> Get() const {
      static_assert(kShiftMask >= 0 && kShiftMask <= UCHAR_MAX,
                    "kShiftMask must fit in a uint8_t");
      // Avoid expanding the width of Shift else the compiler will insert an
      // additional instruction to zero out the upper bits on the critical path
      // of alloc / free.  Not zeroing out the bits is safe because both ARM and
      // x86 only use the lowest byte for shift count in variable shifts.
      return {reinterpret_cast<TcmallocSlab::Slabs*>(raw_ & kSlabsMask),
              static_cast<Shift>(raw_ & kShiftMask)};
    }

    uintptr_t Raw() const {
      // We depend on this in PushBatch/PopBatch.
      static_assert(kShiftMask == 0xFF);
      static_assert(kSlabsMask ==
                    static_cast<size_t>(TCMALLOC_PERCPU_SLABS_MASK));
      return raw_;
    }

   private:
    uintptr_t raw_;
  };

  // Slab header (packed, atomically updated 64-bit).
  // All {begin, current, end} values are pointer offsets from per-CPU region
  // start. The slot array is in [begin, end), and the occupied slots are in
  // [begin, current).
  struct Header {
    // The end offset of the currently occupied slots.
    uint16_t current;
    // Copy of end. Updated by Shrink/Grow, but is not overwritten by Drain.
    uint16_t end_copy;
    // Lock updates only begin and end with a 32-bit write.
    union {
      struct {
        // The begin offset of the slot array for this size class.
        uint16_t begin;
        // The end offset of the slot array for this size class.
        uint16_t end;
      };
      uint32_t lock_update;
    };

    // Lock is used by Drain to stop concurrent mutations of the Header.
    // Lock sets begin to 0xffff and end to 0, which makes Push and Pop fail
    // regardless of current value.
    bool IsLocked() const;
    void Lock();
  };

  // We cast Header to std::atomic<int64_t>.
  static_assert(sizeof(Header) == sizeof(std::atomic<int64_t>),
                "bad Header size");

  // It's important that we use consistent values for slabs/shift rather than
  // loading from the atomic repeatedly whenever we use one of the values.
  ABSL_MUST_USE_RESULT std::pair<Slabs*, Shift> GetSlabsAndShift(
      std::memory_order order) const {
    return slabs_and_shift_.load(order).Get();
  }

  static Slabs* CpuMemoryStart(Slabs* slabs, Shift shift, int cpu);
  static std::atomic<int64_t>* GetHeader(Slabs* slabs, Shift shift, int cpu,
                                         size_t size_class);
  static Header LoadHeader(std::atomic<int64_t>* hdrp);
  static void StoreHeader(std::atomic<int64_t>* hdrp, Header hdr);
  static void LockHeader(Slabs* slabs, Shift shift, int cpu, size_t size_class);
  static int CompareAndSwapHeader(int cpu, std::atomic<int64_t>* hdrp,
                                  Header old, Header hdr,
                                  size_t virtual_cpu_id_offset);
  // <begins> is an array of the <begin> values for each size class.
  static void DrainCpu(Slabs* slabs, Shift shift, int cpu, uint16_t* begins,
                       DrainHandler drain_handler);
  // Stops concurrent mutations from occurring for <cpu> by locking the
  // corresponding headers. All allocations/deallocations will miss this cache
  // for <cpu> until the headers are unlocked.
  static void StopConcurrentMutations(Slabs* slabs, Shift shift, int cpu,
                                      size_t virtual_cpu_id_offset);

  // Implementation of InitCpu() allowing for reuse in ResizeSlabs().
  static void InitCpuImpl(Slabs* slabs, Shift shift, int cpu,
                          size_t virtual_cpu_id_offset,
                          absl::FunctionRef<size_t(size_t)> capacity);

  // We store both a pointer to the array of slabs and the shift value together
  // so that we can atomically update both with a single store.
  std::atomic<SlabsAndShift> slabs_and_shift_{};
  // This is in units of bytes.
  size_t virtual_cpu_id_offset_ = offsetof(kernel_rseq, cpu_id);
  // In ResizeSlabs, we need to allocate space to store begin offsets on the
  // arena. We reuse this space here.
  uint16_t (*resize_begins_)[NumClasses] = nullptr;
};

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::Length(int cpu,
                                               size_t size_class) const {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
  return hdr.IsLocked() ? 0 : hdr.current - hdr.begin;
}

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::Capacity(int cpu,
                                                 size_t size_class) const {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
  return hdr.IsLocked() ? 0 : hdr.end - hdr.begin;
}

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::Grow(
    int cpu, size_t size_class, size_t len,
    absl::FunctionRef<size_t(uint8_t)> max_capacity) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t max_cap = max_capacity(ToUint8(shift));
  const size_t virtual_cpu_id_offset = virtual_cpu_id_offset_;
  std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
  for (;;) {
    Header old = LoadHeader(hdrp);
    // We need to check for `old.begin == 0` because `slabs` may have been
    // MADV_DONTNEEDed after a call to ResizeSlabs().
    if (old.IsLocked() || old.end - old.begin == max_cap || old.begin == 0) {
      return 0;
    }
    uint16_t n = std::min<uint16_t>(len, max_cap - (old.end - old.begin));
    Header hdr = old;
    hdr.end += n;
    hdr.end_copy += n;
    const int ret =
        CompareAndSwapHeader(cpu, hdrp, old, hdr, virtual_cpu_id_offset);
    if (ret == cpu) {
      return n;
    } else if (ret >= 0) {
      return 0;
    }
  }
}

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::Shrink(int cpu, size_t size_class,
                                               size_t len) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t virtual_cpu_id_offset = virtual_cpu_id_offset_;
  std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
  for (;;) {
    Header old = LoadHeader(hdrp);
    // We need to check for `old.begin == 0` because `slabs` may have been
    // MADV_DONTNEEDed after a call to ResizeSlabs().
    if (old.IsLocked() || old.current == old.end || old.begin == 0) {
      return 0;
    }
    uint16_t n = std::min<uint16_t>(len, old.end - old.current);
    Header hdr = old;
    hdr.end -= n;
    hdr.end_copy -= n;
    const int ret =
        CompareAndSwapHeader(cpu, hdrp, old, hdr, virtual_cpu_id_offset);
    if (ret == cpu) {
      return n;
    } else if (ret >= 0) {
      return 0;
    }
  }
}

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__x86_64__)
template <size_t NumClasses>
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE int TcmallocSlab_Internal_Push(
    typename TcmallocSlab<NumClasses>::Slabs* slabs, size_t size_class,
    void* item, Shift shift, OverflowHandler overflow_handler, void* arg,
    const size_t virtual_cpu_id_offset) {
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO
  asm goto(
#else
  bool overflow;
  asm volatile(
#endif
      // __rseq_cs only needs to be writeable to allow for relocations.
      ".pushsection __rseq_cs, \"aw?\"\n"
      ".balign 32\n"
      ".local __rseq_cs_TcmallocSlab_Internal_Push_%=\n"
      ".type __rseq_cs_TcmallocSlab_Internal_Push_%=,@object\n"
      ".size __rseq_cs_TcmallocSlab_Internal_Push_%=,32\n"
      "__rseq_cs_TcmallocSlab_Internal_Push_%=:\n"
      ".long 0x0\n"
      ".long 0x0\n"
      ".quad 4f\n"
      ".quad 5f - 4f\n"
      ".quad 2f\n"
      ".popsection\n"
#if !defined(__clang_major__) || __clang_major__ >= 9
      ".reloc 0, R_X86_64_NONE, 1f\n"
#endif
      ".pushsection __rseq_cs_ptr_array, \"aw?\"\n"
      "1:\n"
      ".balign 8;"
      ".quad __rseq_cs_TcmallocSlab_Internal_Push_%=\n"
      // Force this section to be retained.  It is for debugging, but is
      // otherwise not referenced.
      ".popsection\n"
      ".pushsection .text.unlikely, \"ax?\"\n"
      ".byte 0x0f, 0x1f, 0x05\n"
      ".long %c[rseq_sig]\n"
      ".local TcmallocSlab_Internal_Push_trampoline_%=\n"
      ".type TcmallocSlab_Internal_Push_trampoline_%=,@function\n"
      "TcmallocSlab_Internal_Push_trampoline_%=:\n"
      "2:\n"
      "jmp 3f\n"
      ".size TcmallocSlab_Internal_Push_trampoline_%=, . - "
      "TcmallocSlab_Internal_Push_trampoline_%=;\n"
      ".popsection\n"
      // Prepare
      //
      // TODO(b/151503411):  Pending widespread availability of LLVM's asm
      // goto with output contraints
      // (https://github.com/llvm/llvm-project/commit/23c2a5ce33f0), we can
      // return the register allocations to the compiler rather than using
      // explicit clobbers.  Prior to this, blocks which use asm goto cannot
      // also specify outputs.
      //
      // r10: Scratch
      // r11: Current
      "3:\n"
      "lea __rseq_cs_TcmallocSlab_Internal_Push_%=(%%rip), %%r10\n"
      "mov %%r10, %c[rseq_cs_offset](%[rseq_abi])\n"
      // Start
      "4:\n"
      // scratch = __rseq_abi.cpu_id;
      "movzwl (%[rseq_abi], %[rseq_cpu_offset]), %%r10d\n"
      // scratch = slabs + scratch
      "shlq %b[shift], %%r10\n"
      "add %[slabs], %%r10\n"
      // r11 = slabs->current;
      "movzwq (%%r10, %[size_class], 8), %%r11\n"
      // if (ABSL_PREDICT_FALSE(r11 >= slabs->end)) { goto overflow_label; }
      "cmp 6(%%r10, %[size_class], 8), %%r11w\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO
      "jae %l[overflow_label]\n"
#else
      "jae 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccae)
  // If so, the above code needs to explicitly set a ccae return value.
#endif
      "mov %[item], (%%r10, %%r11, 8)\n"
      "lea 1(%%r11), %%r11\n"
      "mov %%r11w, (%%r10, %[size_class], 8)\n"
      // Commit
      "5:\n"
      :
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO
      [overflow] "=@ccae"(overflow)
#endif
      : [rseq_abi] "r"(&__rseq_abi),
        [rseq_cs_offset] "n"(offsetof(kernel_rseq, rseq_cs)),
        [rseq_cpu_offset] "r"(virtual_cpu_id_offset),
        [rseq_sig] "in"(TCMALLOC_PERCPU_RSEQ_SIGNATURE),
        // We use "c" for shift because shl requires the c register.
        [shift] "c"(ToUint8(shift)), [slabs] "r"(slabs),
        [size_class] "r"(size_class), [item] "r"(item)
      : "cc", "memory", "r10", "r11"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO
      : overflow_label
#endif
  );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO
  if (ABSL_PREDICT_FALSE(overflow)) {
    goto overflow_label;
  }
#endif
  return 0;
overflow_label:
  // As of 3/2020, LLVM's asm goto (even with output constraints) only provides
  // values for the fallthrough path.  The values on the taken branches are
  // undefined.
  int cpu = VirtualRseqCpuId(virtual_cpu_id_offset);
  return overflow_handler(cpu, size_class, item, arg);
}
#endif  // defined(__x86_64__)

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
template <size_t NumClasses>
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE int TcmallocSlab_Internal_Push(
    typename TcmallocSlab<NumClasses>::Slabs* slabs, size_t size_class,
    void* item, Shift shift, OverflowHandler overflow_handler, void* arg,
    const size_t virtual_cpu_id_offset) {
  void* region_start;
  uint64_t cpu_id;
  void* end_ptr;
  uintptr_t current;
  uintptr_t end;
  // Multiply size_class by the bytesize of each header
  size_t size_class_lsl3 = size_class * 8;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO
  asm goto(
#else
  bool overflow;
  asm volatile(
#endif
      // __rseq_cs only needs to be writeable to allow for relocations.
      ".pushsection __rseq_cs, \"aw?\"\n"
      ".balign 32\n"
      ".local __rseq_cs_TcmallocSlab_Internal_Push_%=\n"
      ".type __rseq_cs_TcmallocSlab_Internal_Push_%=,@object\n"
      ".size __rseq_cs_TcmallocSlab_Internal_Push_%=,32\n"
      "__rseq_cs_TcmallocSlab_Internal_Push_%=:\n"
      ".long 0x0\n"
      ".long 0x0\n"
      ".quad 4f\n"
      ".quad 5f - 4f\n"
      ".quad 2f\n"
      ".popsection\n"
#if !defined(__clang_major__) || __clang_major__ >= 9
      ".reloc 0, R_AARCH64_NONE, 1f\n"
#endif
      ".pushsection __rseq_cs_ptr_array, \"aw?\"\n"
      "1:\n"
      ".balign 8;"
      ".quad __rseq_cs_TcmallocSlab_Internal_Push_%=\n"
      // Force this section to be retained.  It is for debugging, but is
      // otherwise not referenced.
      ".popsection\n"
      ".pushsection .text.unlikely, \"ax?\"\n"
      ".long %c[rseq_sig]\n"
      ".local TcmallocSlab_Internal_Push_trampoline_%=\n"
      ".type TcmallocSlab_Internal_Push_trampoline_%=,@function\n"
      "TcmallocSlab_Internal_Push_trampoline_%=:\n"
      "2:\n"
      "b 3f\n"
      ".popsection\n"
      // Prepare
      //
      // TODO(b/151503411):  Pending widespread availability of LLVM's asm
      // goto with output contraints
      // (https://github.com/llvm/llvm-project/commit/23c2a5ce33f0), we can
      // return the register allocations to the compiler rather than using
      // explicit clobbers.  Prior to this, blocks which use asm goto cannot
      // also specify outputs.
      "3:\n"
      // Use current as scratch here to hold address of this function's
      // critical section
      "adrp %[current], __rseq_cs_TcmallocSlab_Internal_Push_%=\n"
      "add  %[current], %[current], "
      ":lo12:__rseq_cs_TcmallocSlab_Internal_Push_%=\n"
      "str %[current], [%[rseq_abi], %c[rseq_cs_offset]]\n"
      // Start
      "4:\n"
      // cpu_id = __rseq_abi.vcpu_id;
      "ldrh %w[cpu_id], [%[rseq_abi], %[rseq_cpu_offset]]\n"
      // region_start = Start of cpu region
      "lsl %w[region_start], %w[cpu_id], %w[shift]\n"
      "add %[region_start], %[region_start], %[slabs]\n"
      // end_ptr = &(slab_headers[0]->end)
      "add %[end_ptr], %[region_start], #6\n"
      // current = slab_headers[size_class]->current (current index)
      "ldrh %w[current], [%[region_start], %[size_class_lsl3]]\n"
      // end = slab_headers[size_class]->end (end index)
      "ldrh %w[end], [%[end_ptr], %[size_class_lsl3]]\n"
      // if (ABSL_PREDICT_FALSE(end <= current)) { goto overflow_label; }
      "cmp %[end], %[current]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO
      "b.le %l[overflow_label]\n"
#else
      "b.le 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccle)
  // If so, the above code needs to explicitly set a ccle return value.
#endif
      "str %[item], [%[region_start], %[current], LSL #3]\n"
      "add %w[current], %w[current], #1\n"
      "strh %w[current], [%[region_start], %[size_class_lsl3]]\n"
      // Commit
      "5:\n"
      : [end_ptr] "=&r"(end_ptr), [cpu_id] "=&r"(cpu_id),
        [current] "=&r"(current), [end] "=&r"(end),
        [region_start] "=&r"(region_start)

#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO
            ,
        [overflow] "=@ccle"(overflow)
#endif
      : [rseq_cpu_offset] "r"(virtual_cpu_id_offset), [slabs] "r"(slabs),
        [size_class_lsl3] "r"(size_class_lsl3), [item] "r"(item),
        [rseq_abi] "r"(&__rseq_abi), [shift] "r"(ToUint8(shift)),
        // Constants
        [rseq_cs_offset] "n"(offsetof(kernel_rseq, rseq_cs)),
        [rseq_sig] "in"(TCMALLOC_PERCPU_RSEQ_SIGNATURE)
      // Add x16 and x17 as an explicit clobber registers:
      // The RSEQ code above uses non-local branches in the restart sequence
      // which is located inside .text.unlikely. The maximum distance of B
      // and BL branches in ARM is limited to 128MB. If the linker detects
      // the distance being too large, it injects a thunk which may clobber
      // the x16 or x17 register according to the ARMv8 ABI standard.
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO
      : "x16", "x17", "cc", "memory"
      : overflow_label
#else
      : "x16", "x17", "memory"
#endif
  );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO
  if (ABSL_PREDICT_FALSE(overflow)) {
    goto overflow_label;
  }
#endif
  return 0;
overflow_label:
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  // As of 3/2020, LLVM's asm goto (even with output constraints) only provides
  // values for the fallthrough path.  The values on the taken branches are
  // undefined.
  int cpu = VirtualRseqCpuId(virtual_cpu_id_offset);
#else
  // With asm goto--without output constraints--the value of scratch is
  // well-defined by the compiler and our implementation.  As an optimization on
  // this case, we can avoid looking up cpu_id again, by undoing the
  // transformation of cpu_id to the value of scratch.
  int cpu = cpu_id;
#endif
  return overflow_handler(cpu, size_class, item, arg);
}
#endif  // defined (__aarch64__)

template <size_t NumClasses>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE bool TcmallocSlab<NumClasses>::Push(
    size_t size_class, void* item, OverflowHandler overflow_handler,
    void* arg) {
  ASSERT(IsFastNoInit());
  ASSERT(item != nullptr);
  // Speculatively annotate item as released to TSan.  We may not succeed in
  // pushing the item, but if we wait for the restartable sequence to succeed,
  // it may become visible to another thread before we can trigger the
  // annotation.
  TSANRelease(item);
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  return TcmallocSlab_Internal_Push<NumClasses>(slabs, size_class, item, shift,
                                                overflow_handler, arg,
                                                virtual_cpu_id_offset_) >= 0;
#else
  Crash(kCrash, __FILE__, __LINE__,
        "RSEQ Push called on unsupported platform.");
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
template <size_t NumClasses>
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab_Internal_Pop(
    typename TcmallocSlab<NumClasses>::Slabs* slabs, size_t size_class,
    UnderflowHandler underflow_handler, void* arg, Shift shift,
    const size_t virtual_cpu_id_offset) {
  void* prefetch_target;
  void* result;
  void* scratch;
  uintptr_t current;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto
#else
  bool underflow;
  asm
#endif
      (
          // __rseq_cs only needs to be writeable to allow for relocations.
          ".pushsection __rseq_cs, \"aw?\"\n"
          ".balign 32\n"
          ".local __rseq_cs_TcmallocSlab_Internal_Pop_%=\n"
          ".type __rseq_cs_TcmallocSlab_Internal_Pop_%=,@object\n"
          ".size __rseq_cs_TcmallocSlab_Internal_Pop_%=,32\n"
          "__rseq_cs_TcmallocSlab_Internal_Pop_%=:\n"
          ".long 0x0\n"
          ".long 0x0\n"
          ".quad 4f\n"
          ".quad 5f - 4f\n"
          ".quad 2f\n"
          ".popsection\n"
#if !defined(__clang_major__) || __clang_major__ >= 9
          ".reloc 0, R_X86_64_NONE, 1f\n"
#endif
          ".pushsection __rseq_cs_ptr_array, \"aw?\"\n"
          "1:\n"
          ".balign 8;"
          ".quad __rseq_cs_TcmallocSlab_Internal_Pop_%=\n"
          // Force this section to be retained.  It is for debugging, but is
          // otherwise not referenced.
          ".popsection\n"
          ".pushsection .text.unlikely, \"ax?\"\n"
          ".byte 0x0f, 0x1f, 0x05\n"
          ".long %c[rseq_sig]\n"
          ".local TcmallocSlab_Internal_Pop_trampoline_%=\n"
          ".type TcmallocSlab_Internal_Pop_trampoline_%=,@function\n"
          "TcmallocSlab_Internal_Pop_trampoline_%=:\n"
          "2:\n"
          "jmp 3f\n"
          ".size TcmallocSlab_Internal_Pop_trampoline_%=, . - "
          "TcmallocSlab_Internal_Pop_trampoline_%=;\n"
          ".popsection\n"
          // Prepare
          "3:\n"
          "lea __rseq_cs_TcmallocSlab_Internal_Pop_%=(%%rip), %[scratch];\n"
          "mov %[scratch], %c[rseq_cs_offset](%[rseq_abi])\n"
          // Start
          "4:\n"
          // scratch = __rseq_abi.cpu_id;
          "movzwl (%[rseq_abi], %[rseq_cpu_offset]), %k[scratch]\n"
          // scratch = slabs + (scratch << shift)
          "shlq %b[shift], %[scratch]\n"
          "add %[slabs], %[scratch]\n"
          // current = scratch->header[size_class].current;
          "movzwq (%[scratch], %[size_class], 8), %[current]\n"
          // if (ABSL_PREDICT_FALSE(current <=
          //                        scratch->header[size_class].begin))
          //   goto underflow_path;
          "cmp 4(%[scratch], %[size_class], 8), %w[current]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
          "jbe %l[underflow_path]\n"
#else
          "jbe 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccbe)
  // If so, the above code needs to explicitly set a ccbe return value.
#endif
          "mov -16(%[scratch], %[current], 8), %[prefetch_target]\n"
          "movq -8(%[scratch], %[current], 8), %[result]\n"
          "lea -1(%[current]), %[current]\n"
          "mov %w[current], (%[scratch], %[size_class], 8)\n"
          // Commit
          "5:\n"
          : [result] "=&r"(result), [prefetch_target] "=&r"(prefetch_target),
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
            [underflow] "=@ccbe"(underflow),
#endif
            [scratch] "=&r"(scratch), [current] "=&r"(current)
          : [rseq_abi] "r"(&__rseq_abi),
            [rseq_cs_offset] "n"(offsetof(kernel_rseq, rseq_cs)),
            [rseq_cpu_offset] "r"(virtual_cpu_id_offset),
            [rseq_sig] "n"(TCMALLOC_PERCPU_RSEQ_SIGNATURE),
            // We use "c" for shift because shl requires the c register.
            [shift] "c"(ToUint8(shift)), [slabs] "r"(slabs),
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
  TSANAcquire(result);

  PrefetchNextObject(prefetch_target);
  return result;
underflow_path:
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  // As of 3/2020, LLVM's asm goto (even with output constraints) only provides
  // values for the fallthrough path.  The values on the taken branches are
  // undefined.
  int cpu = VirtualRseqCpuId(virtual_cpu_id_offset);
#else
  // With asm goto--without output constraints--the value of scratch is
  // well-defined by the compiler and our implementation.  As an optimization on
  // this case, we can avoid looking up cpu_id again, by undoing the
  // transformation of cpu_id to the value of scratch.
  int cpu =
      (reinterpret_cast<char*>(scratch) - reinterpret_cast<char*>(slabs)) >>
      ToUint8(shift);
#endif
  return underflow_handler(cpu, size_class, arg);
}
#endif  // defined(__x86_64__)

#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ && defined(__aarch64__)
template <size_t NumClasses>
static inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab_Internal_Pop(
    typename TcmallocSlab<NumClasses>::Slabs* slabs, size_t size_class,
    UnderflowHandler underflow_handler, void* arg, Shift shift,
    const size_t virtual_cpu_id_offset) {
  void* result;
  void* region_start;
  uint64_t cpu_id;
  void* prefetch;
  uintptr_t current;
  uintptr_t new_current;
  uintptr_t begin;
  // The addresses we load/store for %[current] and %[end] in the slab headers
  // can be computed with loads of the form:
  //   BASE:   (CPU_num << %[shift]) + %[slabs]
  //   OFFSET: (%[size_class]<<3)
  // We can speed up these loads by transforming them into
  //   BASE:   (CPU_num << %[shift])
  //   OFFSET: %[slabs] + (%[size_class]<<3)
  // This removes the addition of %[slabs] from the critical path since we can
  // compute the OFFSET value while the load of CPU_num is still in flight.
  // OFFSET for the current index.
  uint64_t slabs_size_class_lsl3;
  // OFFSET for the end index, which is 4 bytes after the current index.
  uint64_t slabs_size_class_lsl3_plus_4;
  // The allocated item we are going to return is located one item before
  // %current.  I.e.
  //   BASE:   (CPU_num << %[shift]) + %[slabs]
  //   OFFSET: ((%[current] - 1)<<3)
  // If we transform this as follows
  //   BASE:   (CPU_num << %[shift]) + %[slabs] - 8
  //   OFFSET: (%[current]<<3)
  // we speed this load up as it removes the decrement of %[current] from the
  // critical path
  uint64_t slabs_minus_8;
  uint64_t region_start_slabs_minus_8;
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  asm goto
#else
  bool underflow;
  asm
#endif
      (
          // __rseq_cs only needs to be writeable to allow for relocations.
          ".pushsection __rseq_cs, \"aw?\"\n"
          ".balign 32\n"
          ".local __rseq_cs_TcmallocSlab_Internal_Pop_%=\n"
          ".type __rseq_cs_TcmallocSlab_Internal_Pop_%=,@object\n"
          ".size __rseq_cs_TcmallocSlab_Internal_Pop_%=,32\n"
          "__rseq_cs_TcmallocSlab_Internal_Pop_%=:\n"
          ".long 0x0\n"
          ".long 0x0\n"
          ".quad 4f\n"
          ".quad 5f - 4f\n"
          ".quad 2f\n"
          ".popsection\n"
#if !defined(__clang_major__) || __clang_major__ >= 9
          ".reloc 0, R_AARCH64_NONE, 1f\n"
#endif
          ".pushsection __rseq_cs_ptr_array, \"aw?\"\n"
          "1:\n"
          ".balign 8;"
          ".quad __rseq_cs_TcmallocSlab_Internal_Pop_%=\n"
          // Force this section to be retained.  It is for debugging, but is
          // otherwise not referenced.
          ".popsection\n"
          ".pushsection .text.unlikely, \"ax?\"\n"
          ".long %c[rseq_sig]\n"
          ".local TcmallocSlab_Internal_Pop_trampoline_%=\n"
          ".type TcmallocSlab_Internal_Pop_trampoline_%=,@function\n"
          "TcmallocSlab_Internal_Pop_trampoline_%=:\n"
          "2:\n"
          "b 3f\n"
          ".popsection\n"
          // Prepare
          "3:\n"
          // Precompute OFFSET for %[current]
          "add %[slabs_size_class_lsl3], %[slabs], %[size_class], LSL #3\n"
          // Precompute OFFSET for %[end] (four bytes past %[current] OFFSET)
          "add %[slabs_size_class_lsl3_plus_4], %[slabs_size_class_lsl3], #4\n"
          // Use current as scratch here to hold address of this function's
          // critical section
          "adrp %[current], __rseq_cs_TcmallocSlab_Internal_Pop_%=\n"
          // First half of precomputing the allocated item BASE (%[slabs] - 8)
          "sub %[slabs_minus_8], %[slabs], #8\n"
          "add  %[current], %[current], "
          ":lo12:__rseq_cs_TcmallocSlab_Internal_Pop_%=\n"
          "str %[current], [%[rseq_abi], %c[rseq_cs_offset]]\n"
          // Start
          "4:\n"
          // cpu_id = __rseq_abi.vcpu_id;
          "ldrh %w[cpu_id], [%[rseq_abi], %[rseq_cpu_offset]]\n"
          // region_start = Start of cpu region
          "lsl %w[region_start], %w[cpu_id], %w[shift]\n"
          // Second half of precomputing the allocated item BASE
          "add %[region_start_slabs_minus_8], %[slabs_minus_8], "
          "%[region_start]\n"
          // current = slab_headers[size_class]->current (current index)
          "ldrh %w[current], [%[region_start], %[slabs_size_class_lsl3]]\n"
          // current--
          "sub %w[new_current], %w[current], #1\n"
          // begin = slab_headers[size_class]->begin (begin index)
          "ldrh %w[begin], [%[region_start], %[slabs_size_class_lsl3_plus_4]]\n"
          // if (ABSL_PREDICT_FALSE(begin >= current)) { goto underflow_path; }
          "cmp %w[begin], %w[current]\n"
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
          "b.ge %l[underflow_path]\n"
#else
          "b.ge 5f\n"
  // Important! code below this must not affect any flags (i.e.: ccge)
  // If so, the above code needs to explicitly set a ccge return value.
#endif
          "ldr %[result], [%[region_start_slabs_minus_8], %[current], LSL #3]\n"
          "ldr %[prefetch], [%[region_start_slabs_minus_8], %[new_current],"
          "LSL #3]\n"
          "strh %w[new_current], [%[region_start], %[slabs_size_class_lsl3]]\n"
          // Commit
          "5:\n"
          :
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
          [underflow] "=@ccge"(underflow),
#endif
          [result] "=&r"(result), [prefetch] "=&r"(prefetch),
          // Temps
          [cpu_id] "=&r"(cpu_id), [region_start] "=&r"(region_start),
          [region_start_slabs_minus_8] "=&r"(region_start_slabs_minus_8),
          [slabs_minus_8] "=&r"(slabs_minus_8),
          [slabs_size_class_lsl3_plus_4] "=&r"(slabs_size_class_lsl3_plus_4),
          [slabs_size_class_lsl3] "=&r"(slabs_size_class_lsl3),
          [begin] "=&r"(begin), [current] "=&r"(current),
          [new_current] "=&r"(new_current)
          // Real inputs
          : [rseq_cpu_offset] "r"(virtual_cpu_id_offset), [slabs] "r"(slabs),
            [rseq_abi] "r"(&__rseq_abi), [shift] "r"(ToUint8(shift)),
            [size_class] "r"(size_class),
            // constants
            [rseq_cs_offset] "in"(offsetof(kernel_rseq, rseq_cs)),
            [rseq_sig] "in"(TCMALLOC_PERCPU_RSEQ_SIGNATURE)
          // Add x16 and x17 as an explicit clobber registers:
          // The RSEQ code above uses non-local branches in the restart sequence
          // which is located inside .text.unlikely. The maximum distance of B
          // and BL branches in ARM is limited to 128MB. If the linker detects
          // the distance being too large, it injects a thunk which may clobber
          // the x16 or x17 register according to the ARMv8 ABI standard.
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
          : "x16", "x17", "cc", "memory"
          : underflow_path
#else
          : "x16", "x17", "memory"
#endif
      );
#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  if (ABSL_PREDICT_FALSE(underflow)) {
    goto underflow_path;
  }
#endif
  TSANAcquire(result);
  PrefetchNextObject(prefetch);
  return result;
underflow_path:
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ_ASM_GOTO_OUTPUT
  // As of 3/2020, LLVM's asm goto (even with output constraints) only provides
  // values for the fallthrough path.  The values on the taken branches are
  // undefined.
  int cpu = VirtualRseqCpuId(virtual_cpu_id_offset);
#else
  // With asm goto--without output constraints--the value of scratch is
  // well-defined by the compiler and our implementation.  As an optimization on
  // this case, we can avoid looking up cpu_id again, by undoing the
  // transformation of cpu_id to the value of scratch.
  int cpu = cpu_id;
#endif
  return underflow_handler(cpu, size_class, arg);
}
#endif  // defined(__aarch64__)

template <size_t NumClasses>
inline ABSL_ATTRIBUTE_ALWAYS_INLINE void* TcmallocSlab<NumClasses>::Pop(
    size_t size_class, UnderflowHandler underflow_handler, void* arg) {
  ASSERT(IsFastNoInit());
#if TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  return TcmallocSlab_Internal_Pop<NumClasses>(
      slabs, size_class, underflow_handler, arg, shift, virtual_cpu_id_offset_);
#else
  Crash(kCrash, __FILE__, __LINE__, "RSEQ Pop called on unsupported platform.");
#endif
}

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::PushBatch(size_t size_class,
                                                  void** batch, size_t len) {
  ASSERT(len != 0);
  // We need to annotate batch[...] as released before running the restartable
  // sequence, since those objects become visible to other threads the moment
  // the restartable sequence is complete and before the annotation potentially
  // runs.
  //
  // This oversynchronizes slightly, since PushBatch may succeed only partially.
  TSANReleaseBatch(batch, len);
  return TcmallocSlab_Internal_PushBatch(
      size_class, batch, len,
      slabs_and_shift_.load(std::memory_order_relaxed).Raw(),
      virtual_cpu_id_offset_);
}

template <size_t NumClasses>
inline size_t TcmallocSlab<NumClasses>::PopBatch(size_t size_class,
                                                 void** batch, size_t len) {
  ASSERT(len != 0);
  const size_t n = TcmallocSlab_Internal_PopBatch(
      size_class, batch, len,
      slabs_and_shift_.load(std::memory_order_relaxed).Raw(),
      virtual_cpu_id_offset_);
  ASSERT(n <= len);

  // PopBatch is implemented in assembly, msan does not know that the returned
  // batch is initialized.
  ANNOTATE_MEMORY_IS_INITIALIZED(batch, n * sizeof(batch[0]));
  TSANAcquireBatch(batch, n);
  return n;
}

template <size_t NumClasses>
inline auto TcmallocSlab<NumClasses>::CpuMemoryStart(Slabs* slabs, Shift shift,
                                                     int cpu) -> Slabs* {
  char* const bytes = reinterpret_cast<char*>(slabs);
  return reinterpret_cast<Slabs*>(&bytes[cpu << ToUint8(shift)]);
}

template <size_t NumClasses>
inline std::atomic<int64_t>* TcmallocSlab<NumClasses>::GetHeader(
    Slabs* slabs, Shift shift, int cpu, size_t size_class) {
  return &CpuMemoryStart(slabs, shift, cpu)->header[size_class];
}

template <size_t NumClasses>
inline auto TcmallocSlab<NumClasses>::LoadHeader(std::atomic<int64_t>* hdrp)
    -> Header {
  return absl::bit_cast<Header>(hdrp->load(std::memory_order_relaxed));
}

template <size_t NumClasses>
inline void TcmallocSlab<NumClasses>::StoreHeader(std::atomic<int64_t>* hdrp,
                                                  Header hdr) {
  hdrp->store(absl::bit_cast<int64_t>(hdr), std::memory_order_relaxed);
}

template <size_t NumClasses>
inline void TcmallocSlab<NumClasses>::LockHeader(Slabs* slabs, Shift shift,
                                                 int cpu, size_t size_class) {
  // Note: this reinterpret_cast and write in Lock lead to undefined
  // behavior, because the actual object type is std::atomic<int64_t>. But
  // C++ does not allow to legally express what we need here: atomic writes
  // of different sizes.
  reinterpret_cast<Header*>(GetHeader(slabs, shift, cpu, size_class))->Lock();
}

template <size_t NumClasses>
inline int TcmallocSlab<NumClasses>::CompareAndSwapHeader(
    int cpu, std::atomic<int64_t>* hdrp, Header old, Header hdr,
    const size_t virtual_cpu_id_offset) {
#if __SIZEOF_POINTER__ == 8
  const int64_t old_raw = absl::bit_cast<int64_t>(old);
  const int64_t new_raw = absl::bit_cast<int64_t>(hdr);
  return CompareAndSwapUnsafe(cpu, hdrp, static_cast<intptr_t>(old_raw),
                              static_cast<intptr_t>(new_raw),
                              virtual_cpu_id_offset);
#else
  Crash(kCrash, __FILE__, __LINE__, "This architecture is not supported.");
#endif
}

template <size_t NumClasses>
inline void TcmallocSlab<NumClasses>::DrainCpu(Slabs* slabs, Shift shift,
                                               int cpu, uint16_t* begins,
                                               DrainHandler drain_handler) {
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    Header header = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
    const size_t size = header.current - begins[size_class];
    const size_t cap = header.end_copy - begins[size_class];
    void** batch = reinterpret_cast<void**>(GetHeader(slabs, shift, cpu, 0) +
                                            begins[size_class]);
    TSANAcquireBatch(batch, size);
    drain_handler(cpu, size_class, batch, size, cap);
  }
}

template <size_t NumClasses>
inline void TcmallocSlab<NumClasses>::StopConcurrentMutations(
    Slabs* slabs, Shift shift, int cpu, size_t virtual_cpu_id_offset) {
  for (bool done = false; !done;) {
    for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
      LockHeader(slabs, shift, cpu, size_class);
    }
    FenceCpu(cpu, virtual_cpu_id_offset);
    done = true;
    for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
      Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
      if (!hdr.IsLocked()) {
        // Header was overwritten by Grow/Shrink. Retry.
        done = false;
        break;
      }
    }
  }
}

template <size_t NumClasses>
inline bool TcmallocSlab<NumClasses>::Header::IsLocked() const {
  if (begin == 0xffffu) ASSERT(end == 0 && "begin == 0xffffu -> end == 0");
  return begin == 0xffffu;
}

template <size_t NumClasses>
inline void TcmallocSlab<NumClasses>::Header::Lock() {
  // Write 0xffff to begin and 0 to end. This blocks new Push'es and Pop's.
  // Note: we write only 4 bytes. The first 4 bytes are left intact.
  // See Drain method for details. tl;dr: C++ does not allow us to legally
  // express this without undefined behavior.
  std::atomic<int32_t>* p =
      reinterpret_cast<std::atomic<int32_t>*>(&lock_update);
  Header hdr;
  hdr.begin = 0xffffu;
  hdr.end = 0;
  p->store(absl::bit_cast<int32_t>(hdr.lock_update), std::memory_order_relaxed);
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::Init(Slabs* slabs,
                                    absl::FunctionRef<size_t(size_t)> capacity,
                                    Shift shift) {
  if (UsingFlatVirtualCpus()) {
    virtual_cpu_id_offset_ = offsetof(kernel_rseq, vcpu_id);
  }

  slabs_and_shift_.store({slabs, shift}, std::memory_order_relaxed);
  const int num_cpus = absl::base_internal::NumCPUs();
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    Slabs* curr_slab = CpuMemoryStart(slabs, shift, cpu);
    void** elems = curr_slab->mem;

    for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
      size_t cap = capacity(size_class);
      CHECK_CONDITION(static_cast<uint16_t>(cap) == cap);

      if (cap == 0) {
        continue;
      }

      // One extra element for prefetch
      const size_t num_pointers = cap + 1;

      elems += num_pointers;
      const size_t bytes_used_on_curr_slab =
          reinterpret_cast<char*>(elems) - reinterpret_cast<char*>(curr_slab);
      if (bytes_used_on_curr_slab > (1 << ToUint8(shift))) {
        Crash(kCrash, __FILE__, __LINE__, "per-CPU memory exceeded, have ",
              1 << ToUint8(shift), " need ", bytes_used_on_curr_slab);
      }
    }
  }
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::InitCpu(
    int cpu, absl::FunctionRef<size_t(size_t)> capacity) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  InitCpuImpl(slabs, shift, cpu, virtual_cpu_id_offset_, capacity);
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::InitCpuImpl(
    Slabs* slabs, Shift shift, int cpu, size_t virtual_cpu_id_offset,
    absl::FunctionRef<size_t(size_t)> capacity) {
  // Phase 1: stop concurrent mutations for <cpu>. Locking ensures that there
  // exists no value of current such that begin < current.
  StopConcurrentMutations(slabs, shift, cpu, virtual_cpu_id_offset);

  // Phase 2: Initialize prefetch target and compute the offsets for the
  // boundaries of each size class' cache.
  Slabs* curr_slab = CpuMemoryStart(slabs, shift, cpu);
  void** elems = curr_slab->mem;
  uint16_t begin[NumClasses];
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    size_t cap = capacity(size_class);
    CHECK_CONDITION(static_cast<uint16_t>(cap) == cap);

    if (cap) {
      // In Pop() we prefetch the item a subsequent Pop() would return; this is
      // slow if it's not a valid pointer. To avoid this problem when popping
      // the last item, keep one fake item before the actual ones (that points,
      // safely, to itself).
      *elems = elems;
      ++elems;
    }

    size_t offset = elems - reinterpret_cast<void**>(curr_slab);
    CHECK_CONDITION(static_cast<uint16_t>(offset) == offset);
    begin[size_class] = offset;

    elems += cap;
    const size_t bytes_used_on_curr_slab =
        reinterpret_cast<char*>(elems) - reinterpret_cast<char*>(curr_slab);
    if (bytes_used_on_curr_slab > (1 << ToUint8(shift))) {
      Crash(kCrash, __FILE__, __LINE__, "per-CPU memory exceeded, have ",
            1 << ToUint8(shift), " need ", bytes_used_on_curr_slab);
    }
  }

  // Phase 3: Store current.  No restartable sequence will proceed
  // (successfully) as !(begin < current) for all size classes.
  //
  // We must write current and complete a fence before storing begin and end
  // (b/147974701).
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr = LoadHeader(hdrp);
    hdr.current = begin[size_class];
    StoreHeader(hdrp, hdr);
  }
  FenceCpu(cpu, virtual_cpu_id_offset);

  // Phase 4: Allow access to this cache.
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    Header hdr;
    hdr.current = begin[size_class];
    hdr.begin = begin[size_class];
    hdr.end = begin[size_class];
    hdr.end_copy = begin[size_class];
    StoreHeader(GetHeader(slabs, shift, cpu, size_class), hdr);
  }
}

template <size_t NumClasses>
auto TcmallocSlab<NumClasses>::ResizeSlabs(
    Shift new_shift, Slabs* new_slabs,
    absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
    absl::FunctionRef<size_t(size_t)> capacity,
    absl::FunctionRef<bool(size_t)> populated, DrainHandler drain_handler)
    -> ResizeSlabsInfo {
  // Phase 1: Initialize any cores in the new slab that have already been
  // populated in the old slab.
  const auto [old_slabs, old_shift] =
      GetSlabsAndShift(std::memory_order_relaxed);
  ASSERT(new_shift != old_shift);
  const size_t virtual_cpu_id_offset = virtual_cpu_id_offset_;
  const int num_cpus = absl::base_internal::NumCPUs();
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    if (populated(cpu)) {
      InitCpuImpl(new_slabs, new_shift, cpu, virtual_cpu_id_offset, capacity);
    }
  }

  // Phase 2: Collect all `begin`s (these are not mutated by anybody else thanks
  // to the cpu locks) and stop concurrent mutations for all populated CPUs and
  // size classes by locking all the headers.
  // Note: we can't do regular malloc here for resize_begins_ because we may be
  // holding the CpuCache spinlocks. We allocate memory on the arena and keep
  // the pointer for reuse.
  const size_t begins_size = sizeof(uint16_t) * NumClasses * num_cpus;
  if (resize_begins_ == nullptr) {
    resize_begins_ = reinterpret_cast<uint16_t(*)[NumClasses]>(
        alloc(begins_size, std::align_val_t{alignof(uint16_t)}));
  }
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    if (!populated(cpu)) continue;
    for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
      Header header =
          LoadHeader(GetHeader(old_slabs, old_shift, cpu, size_class));
      CHECK_CONDITION(!header.IsLocked());
      resize_begins_[cpu][size_class] = header.begin;
    }
    StopConcurrentMutations(old_slabs, old_shift, cpu, virtual_cpu_id_offset);
  }

  // Phase 3: Atomically update slabs and shift.
  slabs_and_shift_.store({new_slabs, new_shift}, std::memory_order_relaxed);

  // Phase 4: Return pointers from the old slab to the TransferCache.
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    if (!populated(cpu)) continue;
    DrainCpu(old_slabs, old_shift, cpu, &resize_begins_[cpu][0], drain_handler);
  }

  // Phase 5: Update all the `current` values to 0 and fence all CPUs. In RSEQ
  // for Pop/PopBatch, we load current before loading begin so it's possible to
  // get an interleaving of: (Thread 1) load current (>0); (Thread 2)
  // MADVISE_DONTNEED away slabs; (Thread 1) load begin (now ==0), see
  // begin<current so we can Pop.
  // NOTE: we do this after DrainCpu because DrainCpu relies on headers having
  // accurate `current` values.
  for (size_t cpu = 0; cpu < num_cpus; ++cpu) {
    if (!populated(cpu)) continue;
    for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
      std::atomic<int64_t>* header_ptr =
          GetHeader(old_slabs, old_shift, cpu, size_class);
      Header header = LoadHeader(header_ptr);
      header.current = 0;
      StoreHeader(header_ptr, header);
    }
  }
  FenceAllCpus();

  return {old_slabs, GetSlabsAllocSize(old_shift, num_cpus)};
}

template <size_t NumClasses>
void* TcmallocSlab<NumClasses>::Destroy(
    absl::FunctionRef<void(void*, size_t, std::align_val_t)> free) {
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  free(slabs, GetSlabsAllocSize(shift, absl::base_internal::NumCPUs()),
       kPhysicalPageAlign);
  slabs_and_shift_.store({nullptr, shift}, std::memory_order_relaxed);
  return slabs;
}

template <size_t NumClasses>
size_t TcmallocSlab<NumClasses>::ShrinkOtherCache(
    int cpu, size_t size_class, size_t len, ShrinkHandler shrink_handler) {
  ASSERT(cpu >= 0);
  ASSERT(cpu < absl::base_internal::NumCPUs());
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t virtual_cpu_id_offset = virtual_cpu_id_offset_;

  // Phase 1: Collect begin as it will be overwritten by the lock.
  std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
  Header hdr = LoadHeader(hdrp);
  CHECK_CONDITION(!hdr.IsLocked());
  const uint16_t begin = hdr.begin;

  // Phase 2: stop concurrent mutations for <cpu> for size class <size_class>.
  do {
    LockHeader(slabs, shift, cpu, size_class);
    FenceCpu(cpu, virtual_cpu_id_offset);
    hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
    // If the header was overwritten in Grow/Shrink, then we need to try again.
  } while (!hdr.IsLocked());

  // Phase 3: If we do not have len number of items to shrink, we try
  // to pop items from the list first to create enough capacity that can be
  // shrunk. If we pop items, we also execute callbacks.
  //
  // We can't write all 4 fields at once with a single write, because Pop does
  // several non-atomic loads of the fields. Consider that a concurrent Pop
  // loads old current (still pointing somewhere in the middle of the region);
  // then we update all fields with a single write; then Pop loads the updated
  // begin which allows it to proceed; then it decrements current below begin.
  //
  // So we instead first just update current--our locked begin/end guarantee
  // no Push/Pop will make progress.  Once we Fence below, we know no Push/Pop
  // is using the old current, and can safely update begin/end to be an empty
  // slab.

  const uint16_t unused = hdr.end_copy - hdr.current;
  if (unused < len) {
    const uint16_t expected_pop = len - unused;
    const uint16_t actual_pop =
        std::min<uint16_t>(expected_pop, hdr.current - begin);
    void** batch = reinterpret_cast<void**>(GetHeader(slabs, shift, cpu, 0) +
                                            hdr.current - actual_pop);
    TSANAcquireBatch(batch, actual_pop);
    shrink_handler(size_class, batch, actual_pop);
    hdr.current -= actual_pop;
    StoreHeader(hdrp, hdr);
    FenceCpu(cpu, virtual_cpu_id_offset);
  }

  // Phase 4: Shrink the capacity. Use a copy of begin and end_copy to
  // restore the header, shrink it, and return the length by which the
  // region was shrunk.
  hdr.begin = begin;
  const uint16_t to_shrink =
      std::min<uint16_t>(len, hdr.end_copy - hdr.current);
  hdr.end_copy -= to_shrink;
  hdr.end = hdr.end_copy;
  StoreHeader(hdrp, hdr);
  return to_shrink;
}

template <size_t NumClasses>
void TcmallocSlab<NumClasses>::Drain(int cpu, DrainHandler drain_handler) {
  CHECK_CONDITION(cpu >= 0);
  CHECK_CONDITION(cpu < absl::base_internal::NumCPUs());
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  const size_t virtual_cpu_id_offset = virtual_cpu_id_offset_;

  // Push/Pop/Grow/Shrink can be executed concurrently with Drain.
  // That's not an expected case, but it must be handled for correctness.
  // Push/Pop/Grow/Shrink can only be executed on <cpu> and use rseq primitives.
  // Push only updates current. Pop only updates current and end_copy
  // (it mutates only current but uses 4 byte write for performance).
  // Grow/Shrink mutate end and end_copy using 64-bit stores.

  // We attempt to stop all concurrent operations by writing 0xffff to begin
  // and 0 to end. However, Grow/Shrink can overwrite our write, so we do this
  // in a loop until we know that the header is in quiescent state.

  // Phase 1: collect all begin's (these are not mutated by anybody else).
  uint16_t begin[NumClasses];
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    Header hdr = LoadHeader(GetHeader(slabs, shift, cpu, size_class));
    CHECK_CONDITION(!hdr.IsLocked());
    begin[size_class] = hdr.begin;
  }

  // Phase 2: stop concurrent mutations for <cpu>.
  StopConcurrentMutations(slabs, shift, cpu, virtual_cpu_id_offset);

  // Phase 3: execute callbacks.
  DrainCpu(slabs, shift, cpu, begin, drain_handler);

  // Phase 4: reset current to beginning of the region.
  // We can't write all 4 fields at once with a single write, because Pop does
  // several non-atomic loads of the fields. Consider that a concurrent Pop
  // loads old current (still pointing somewhere in the middle of the region);
  // then we update all fields with a single write; then Pop loads the updated
  // begin which allows it to proceed; then it decrements current below begin.
  //
  // So we instead first just update current--our locked begin/end guarantee
  // no Push/Pop will make progress.  Once we Fence below, we know no Push/Pop
  // is using the old current, and can safely update begin/end to be an empty
  // slab.
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr = LoadHeader(hdrp);
    hdr.current = begin[size_class];
    StoreHeader(hdrp, hdr);
  }

  // Phase 5: fence and reset the remaining fields to beginning of the region.
  // This allows concurrent mutations again.
  FenceCpu(cpu, virtual_cpu_id_offset);
  for (size_t size_class = 0; size_class < NumClasses; ++size_class) {
    std::atomic<int64_t>* hdrp = GetHeader(slabs, shift, cpu, size_class);
    Header hdr;
    hdr.current = begin[size_class];
    hdr.begin = begin[size_class];
    hdr.end = begin[size_class];
    hdr.end_copy = begin[size_class];
    StoreHeader(hdrp, hdr);
  }
}

template <size_t NumClasses>
PerCPUMetadataState TcmallocSlab<NumClasses>::MetadataMemoryUsage() const {
  PerCPUMetadataState result;
  const auto [slabs, shift] = GetSlabsAndShift(std::memory_order_relaxed);
  result.virtual_size =
      GetSlabsAllocSize(shift, absl::base_internal::NumCPUs());
  result.resident_size = MInCore::residence(slabs, result.virtual_size);
  return result;
}

}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_PERCPU_TCMALLOC_H_
