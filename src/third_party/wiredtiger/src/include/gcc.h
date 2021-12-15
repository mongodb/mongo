/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define WT_PTRDIFFT_FMT "td" /* ptrdiff_t format string */
#define WT_SIZET_FMT "zu"    /* size_t format string */

/* GCC-specific attributes. */
#define WT_PACKED_STRUCT_BEGIN(name)             \
    /* NOLINTNEXTLINE(misc-macro-parentheses) */ \
    struct __attribute__((__packed__)) name {
#define WT_PACKED_STRUCT_END \
    }                        \
    ;

/*
 * Attribute are only permitted on function declarations, not definitions. This macro is a marker
 * for function definitions that is rewritten by dist/s_prototypes to create extern.h.
 */
#define WT_GCC_FUNC_ATTRIBUTE(x)
#define WT_GCC_FUNC_DECL_ATTRIBUTE(x) __attribute__(x)

/*
 * Atomic writes:
 *
 * WiredTiger requires pointers (void *) and some variables to be read/written
 * atomically, that is, in a single cycle.  This is not write ordering -- to be
 * clear, the requirement is that no partial value can ever be read or written.
 * For example, if 8-bits of a 32-bit quantity were written, then the rest of
 * the 32-bits were written, and another thread of control was able to read the
 * memory location after the first 8-bits were written and before the subsequent
 * 24-bits were written, WiredTiger would break. Or, if two threads of control
 * attempt to write the same location simultaneously, the result must be one or
 * the other of the two values, not some combination of both.
 *
 * To reduce memory requirements, we use a 32-bit type on 64-bit machines, which
 * is OK if the compiler doesn't accumulate two adjacent 32-bit variables into a
 * single 64-bit write, that is, there needs to be a single load/store of the 32
 * bits, not a load/store of 64 bits, where the 64 bits is comprised of two
 * adjacent 32-bit locations.  The problem is when two threads are cooperating
 * (thread X finds 32-bits set to 0, writes in a new value, flushes memory;
 * thread Y reads 32-bits that are non-zero, does some operation, resets the
 * memory location to 0 and flushes). If thread X were to read the 32 bits
 * adjacent to a different 32 bits, and write them both, the two threads could
 * race.  If that can happen, you must increase the size of the memory type to
 * a type guaranteed to be written atomically in a single cycle, without writing
 * an adjacent memory location.
 *
 * WiredTiger additionally requires atomic writes for 64-bit memory locations,
 * and so cannot run on machines with a 32-bit memory bus.
 *
 * We don't depend on writes across cache lines being atomic, and to make sure
 * that never happens, we check address alignment: we know of no architectures
 * with cache lines other than a multiple of 4 bytes in size, so aligned 4-byte
 * accesses will always be in a single cache line.
 *
 * Atomic writes are often associated with memory barriers, implemented by the
 * WT_READ_BARRIER and WT_WRITE_BARRIER macros.  WiredTiger's requirement as
 * described by the Solaris membar_enter description:
 *
 *	No stores from after the memory barrier will reach visibility and
 *	no loads from after the barrier will be resolved before the lock
 *	acquisition reaches global visibility
 *
 * In other words, the WT_WRITE_BARRIER macro must ensure that memory stores by
 * the processor, made before the WT_WRITE_BARRIER call, be visible to all
 * processors in the system before any memory stores by the processor, made
 * after the WT_WRITE_BARRIER call, are visible to any processor.  The
 * WT_READ_BARRIER macro ensures that all loads before the barrier are complete
 * before any loads after the barrier.  The compiler cannot reorder or cache
 * values across a barrier.
 *
 * Lock and unlock operations imply both read and write barriers.  In other
 * words, barriers are not required for values protected by locking.
 *
 * Data locations may also be marked volatile, forcing the compiler to re-load
 * the data on each access.  This is a weaker semantic than barriers provide,
 * only ensuring that the compiler will not cache values.  It makes no ordering
 * guarantees and may have no effect on systems with weaker cache guarantees.
 *
 * In summary, locking > barriers > volatile.
 *
 * To avoid locking shared data structures such as statistics and to permit
 * atomic state changes, we rely on the atomic-add and atomic-cas (compare and
 * swap) operations.
 */

/*
 * We've hit optimization bugs with Clang 3.5 in the past when using the atomic builtins. See
 * http://llvm.org/bugs/show_bug.cgi?id=21499 for details.
 */
#if defined(__clang__) && defined(__clang_major__) && defined(__clang_minor__) && \
  (((__clang_major__ == 3) && (__clang_minor__ <= 5)) || (__clang_major__ < 3))
#error "Clang versions 3.5 and earlier are unsupported by WiredTiger"
#endif

#define WT_ATOMIC_CAS(ptr, oldp, newv) \
    __atomic_compare_exchange_n(ptr, oldp, newv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define WT_ATOMIC_CAS_FUNC(name, vp_arg, old_arg, newv_arg)             \
    static inline bool __wt_atomic_cas##name(vp_arg, old_arg, newv_arg) \
    {                                                                   \
        return (WT_ATOMIC_CAS(vp, &old, newv));                         \
    }
WT_ATOMIC_CAS_FUNC(8, uint8_t *vp, uint8_t old, uint8_t newv)
WT_ATOMIC_CAS_FUNC(v8, volatile uint8_t *vp, uint8_t old, volatile uint8_t newv)
WT_ATOMIC_CAS_FUNC(16, uint16_t *vp, uint16_t old, uint16_t newv)
WT_ATOMIC_CAS_FUNC(32, uint32_t *vp, uint32_t old, uint32_t newv)
WT_ATOMIC_CAS_FUNC(v32, volatile uint32_t *vp, uint32_t old, volatile uint32_t newv)
WT_ATOMIC_CAS_FUNC(i32, int32_t *vp, int32_t old, int32_t newv)
WT_ATOMIC_CAS_FUNC(iv32, volatile int32_t *vp, int32_t old, volatile int32_t newv)
WT_ATOMIC_CAS_FUNC(64, uint64_t *vp, uint64_t old, uint64_t newv)
WT_ATOMIC_CAS_FUNC(v64, volatile uint64_t *vp, uint64_t old, volatile uint64_t newv)
WT_ATOMIC_CAS_FUNC(i64, int64_t *vp, int64_t old, int64_t newv)
WT_ATOMIC_CAS_FUNC(iv64, volatile int64_t *vp, int64_t old, volatile int64_t newv)
WT_ATOMIC_CAS_FUNC(size, size_t *vp, size_t old, size_t newv)

/*
 * __wt_atomic_cas_ptr --
 *     Pointer compare and swap.
 */
static inline bool
__wt_atomic_cas_ptr(void *vp, void *old, void *newv)
{
    return (WT_ATOMIC_CAS((void **)vp, &old, newv));
}

#define WT_ATOMIC_FUNC(name, ret, vp_arg, v_arg)                 \
    static inline ret __wt_atomic_add##name(vp_arg, v_arg)       \
    {                                                            \
        return (__atomic_add_fetch(vp, v, __ATOMIC_SEQ_CST));    \
    }                                                            \
    static inline ret __wt_atomic_fetch_add##name(vp_arg, v_arg) \
    {                                                            \
        return (__atomic_fetch_add(vp, v, __ATOMIC_SEQ_CST));    \
    }                                                            \
    static inline ret __wt_atomic_sub##name(vp_arg, v_arg)       \
    {                                                            \
        return (__atomic_sub_fetch(vp, v, __ATOMIC_SEQ_CST));    \
    }
WT_ATOMIC_FUNC(8, uint8_t, uint8_t *vp, uint8_t v)
WT_ATOMIC_FUNC(v8, uint8_t, volatile uint8_t *vp, volatile uint8_t v)
WT_ATOMIC_FUNC(16, uint16_t, uint16_t *vp, uint16_t v)
WT_ATOMIC_FUNC(32, uint32_t, uint32_t *vp, uint32_t v)
WT_ATOMIC_FUNC(v32, uint32_t, volatile uint32_t *vp, volatile uint32_t v)
WT_ATOMIC_FUNC(i32, int32_t, int32_t *vp, int32_t v)
WT_ATOMIC_FUNC(iv32, int32_t, volatile int32_t *vp, volatile int32_t v)
WT_ATOMIC_FUNC(64, uint64_t, uint64_t *vp, uint64_t v)
WT_ATOMIC_FUNC(v64, uint64_t, volatile uint64_t *vp, volatile uint64_t v)
WT_ATOMIC_FUNC(i64, int64_t, int64_t *vp, int64_t v)
WT_ATOMIC_FUNC(iv64, int64_t, volatile int64_t *vp, volatile int64_t v)
WT_ATOMIC_FUNC(size, size_t, size_t *vp, size_t v)

/* Compile read-write barrier */
#define WT_BARRIER() __asm__ volatile("" ::: "memory")

#if defined(x86_64) || defined(__x86_64__)
/* Pause instruction to prevent excess processor bus usage */
#define WT_PAUSE() __asm__ volatile("pause\n" ::: "memory")
#define WT_FULL_BARRIER()                        \
    do {                                         \
        __asm__ volatile("mfence" ::: "memory"); \
    } while (0)
#define WT_READ_BARRIER()                        \
    do {                                         \
        __asm__ volatile("lfence" ::: "memory"); \
    } while (0)
#define WT_WRITE_BARRIER()                       \
    do {                                         \
        __asm__ volatile("sfence" ::: "memory"); \
    } while (0)

#elif defined(i386) || defined(__i386__)
#define WT_PAUSE() __asm__ volatile("pause\n" ::: "memory")
#define WT_FULL_BARRIER()                                         \
    do {                                                          \
        __asm__ volatile("lock; addl $0, 0(%%esp)" ::: "memory"); \
    } while (0)
#define WT_READ_BARRIER() WT_FULL_BARRIER()
#define WT_WRITE_BARRIER() WT_FULL_BARRIER()

#elif defined(__mips64el__) || defined(__mips__) || defined(__mips64__) || defined(__mips64)
#define WT_PAUSE() __asm__ volatile("pause\n" ::: "memory")
#define WT_FULL_BARRIER()                                                                  \
    do {                                                                                   \
        __asm__ volatile("sync; ld $0, %0" ::"m"(*(long *)0xffffffff80000000) : "memory"); \
    } while (0)
#define WT_READ_BARRIER()                                                                  \
    do {                                                                                   \
        __asm__ volatile("sync; ld $0, %0" ::"m"(*(long *)0xffffffff80000000) : "memory"); \
    } while (0)
#define WT_WRITE_BARRIER()                                                                 \
    do {                                                                                   \
        __asm__ volatile("sync; ld $0, %0" ::"m"(*(long *)0xffffffff80000000) : "memory"); \
    } while (0)

#elif defined(__PPC64__) || defined(PPC64)
/* ori 0,0,0 is the PPC64 noop instruction */
#define WT_PAUSE() __asm__ volatile("ori 0,0,0" ::: "memory")
#define WT_FULL_BARRIER()                      \
    do {                                       \
        __asm__ volatile("sync" ::: "memory"); \
    } while (0)

/* TODO: ISA 2.07 Elemental Memory Barriers would be better,
   specifically mbll, and mbss, but they are not supported by POWER 8 */
#define WT_READ_BARRIER()                        \
    do {                                         \
        __asm__ volatile("lwsync" ::: "memory"); \
    } while (0)
#define WT_WRITE_BARRIER()                       \
    do {                                         \
        __asm__ volatile("lwsync" ::: "memory"); \
    } while (0)

#elif defined(__aarch64__)
/*
 * Use an isb instruction here to be closer to the original x86 pause instruction. The yield
 * instruction that was previously here is a nop that is intended to provide a hint that a
 * thread in a SMT system could yield. This is different from the x86 pause instruction
 * which delays execution by O(100) cycles. The isb will typically delay execution by about
 * 50 cycles so it's a reasonable alternative.
 */
#define WT_PAUSE() __asm__ volatile("isb" ::: "memory")

/*
 * dmb are chosen here because they are sufficient to guarantee the ordering described above. We
 * don't want to use dsbs because they provide a much stronger guarantee of completion which isn't
 * required. Additionally, dsbs synchronize other system activities such as tlb and cache
 * maintenance instructions which is not required in this case.
 *
 * A shareability domain of inner-shareable is selected because all the entities participating in
 * the ordering requirements are CPUs and ordering with respect to other devices or memory-types
 * isn't required.
 */
#define WT_FULL_BARRIER()                         \
    do {                                          \
        __asm__ volatile("dmb ish" ::: "memory"); \
    } while (0)
#define WT_READ_BARRIER()                           \
    do {                                            \
        __asm__ volatile("dsb ishld" ::: "memory"); \
    } while (0)
#define WT_WRITE_BARRIER()                          \
    do {                                            \
        __asm__ volatile("dsb ishst" ::: "memory"); \
    } while (0)

#elif defined(__s390x__)
#define WT_PAUSE() __asm__ volatile("lr 0,0" ::: "memory")
#define WT_FULL_BARRIER()                            \
    do {                                             \
        __asm__ volatile("bcr 15,0\n" ::: "memory"); \
    } while (0)
#define WT_READ_BARRIER() WT_FULL_BARRIER()
#define WT_WRITE_BARRIER() WT_FULL_BARRIER()

#elif defined(__sparc__)
#define WT_PAUSE() __asm__ volatile("rd %%ccr, %%g0" ::: "memory")

#define WT_FULL_BARRIER()                                   \
    do {                                                    \
        __asm__ volatile("membar #StoreLoad" ::: "memory"); \
    } while (0)

/*
 * On UltraSparc machines, TSO is used, and so there is no need for membar. READ_BARRIER =
 * #LoadLoad, and WRITE_BARRIER = #StoreStore are noop.
 */
#define WT_READ_BARRIER()                  \
    do {                                   \
        __asm__ volatile("" ::: "memory"); \
    } while (0)

#define WT_WRITE_BARRIER()                 \
    do {                                   \
        __asm__ volatile("" ::: "memory"); \
    } while (0)

#elif defined(__riscv) && (__riscv_xlen == 64)

/*
 * There is a `pause` instruction which has been recently adopted for RISC-V but it does not appear
 * that compilers support it yet. See:
 *
 * https://riscv.org/announcements/2021/02/
 *    risc-v-international-unveils-fast-track-architecture-
 *    extension-process-and-ratifies-zihintpause-extension
 *
 * Once compiler support is ready, this can and should be replaced with `pause` to enable more
 * efficient spin locks.
 */
#define WT_PAUSE() __asm__ volatile("nop" ::: "memory")

/*
 * The RISC-V fence instruction is documented here:
 *
 * https://five-embeddev.com/riscv-isa-manual/latest/memory.html#sec:mm:fence
 *
 * On RISC-V, the fence instruction takes explicit flags that indicate the predecessor and successor
 * sets. Based on the file comment description of WT_READ_BARRIER and WT_WRITE_BARRIER, those
 * barriers only synchronize read/read and write/write respectively. The predecessor and successor
 * sets here are selected to match that description.
 */
#define WT_FULL_BARRIER()                              \
    do {                                               \
        __asm__ volatile("fence rw, rw" ::: "memory"); \
    } while (0)
#define WT_READ_BARRIER()                            \
    do {                                             \
        __asm__ volatile("fence r, r" ::: "memory"); \
    } while (0)
#define WT_WRITE_BARRIER()                           \
    do {                                             \
        __asm__ volatile("fence w, w" ::: "memory"); \
    } while (0)

#else
#error "No write barrier implementation for this hardware"
#endif
