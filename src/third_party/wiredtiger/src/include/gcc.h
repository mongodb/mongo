/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

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
 * For details on the hardware requirements of WiredTiger see the portability documentation. For
 * details on concurrency primitive usage in WiredTiger see the architecture guide page "WiredTiger
 * concurrency management".
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
WT_ATOMIC_CAS_FUNC(v16, volatile uint16_t *vp, uint16_t old, volatile uint16_t newv)
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

#define WT_ATOMIC_FUNC(name, ret, vp_arg, v_arg)                                                  \
    static inline ret __wt_atomic_add##name(vp_arg, v_arg)                                        \
    {                                                                                             \
        return (__atomic_add_fetch(vp, v, __ATOMIC_SEQ_CST));                                     \
    }                                                                                             \
    static inline ret __wt_atomic_fetch_add##name(vp_arg, v_arg)                                  \
    {                                                                                             \
        return (__atomic_fetch_add(vp, v, __ATOMIC_SEQ_CST));                                     \
    }                                                                                             \
    static inline ret __wt_atomic_sub##name(vp_arg, v_arg)                                        \
    {                                                                                             \
        return (__atomic_sub_fetch(vp, v, __ATOMIC_SEQ_CST));                                     \
    }                                                                                             \
    /*                                                                                            \
     * !!!                                                                                        \
     * The following atomic functions are ATOMIC_RELAXED while the preceding calls are            \
     * ATOMIC_SEQ_CST. Mixing RELAXED and SEQ_CST means we will *not* get sequentially consistent \
     * guarantees.                                                                                \
     * Historically WiredTiger mixed the SEQ_CST calls above with non-atomic accesses to memory,  \
     * and these non-atomic calls are being replaced with atomic calls. Using these new atomic    \
     * functions with SEQ_CST memory ordering comes with a moderate performance cost so we're     \
     * using RELAXED to maintain performance. In future these atomics will need to be reviewed    \
     * and selectively moved to the appropriate memory ordering.                                  \
     */                                                                                           \
    static inline ret __wt_atomic_load##name(vp_arg)                                              \
    {                                                                                             \
        return (__atomic_load_n(vp, __ATOMIC_RELAXED));                                           \
    }                                                                                             \
    static inline void __wt_atomic_store##name(vp_arg, v_arg)                                     \
    {                                                                                             \
        __atomic_store_n(vp, v, __ATOMIC_RELAXED);                                                \
    }
WT_ATOMIC_FUNC(8, uint8_t, uint8_t *vp, uint8_t v)
WT_ATOMIC_FUNC(v8, uint8_t, volatile uint8_t *vp, volatile uint8_t v)
WT_ATOMIC_FUNC(16, uint16_t, uint16_t *vp, uint16_t v)
WT_ATOMIC_FUNC(v16, uint16_t, volatile uint16_t *vp, volatile uint16_t v)
WT_ATOMIC_FUNC(32, uint32_t, uint32_t *vp, uint32_t v)
WT_ATOMIC_FUNC(v32, uint32_t, volatile uint32_t *vp, volatile uint32_t v)
WT_ATOMIC_FUNC(i32, int32_t, int32_t *vp, int32_t v)
WT_ATOMIC_FUNC(iv32, int32_t, volatile int32_t *vp, volatile int32_t v)
WT_ATOMIC_FUNC(64, uint64_t, uint64_t *vp, uint64_t v)
WT_ATOMIC_FUNC(v64, uint64_t, volatile uint64_t *vp, volatile uint64_t v)
WT_ATOMIC_FUNC(i64, int64_t, int64_t *vp, int64_t v)
WT_ATOMIC_FUNC(iv64, int64_t, volatile int64_t *vp, volatile int64_t v)
WT_ATOMIC_FUNC(size, size_t, size_t *vp, size_t v)

/*
 * We can't use the WT_ATOMIC_FUNC macro for booleans as __atomic_add_fetch and __atomic_sub_fetch
 * don't accept booleans when compiling with clang. Define them individually.
 */

/*
 * __wt_atomic_loadbool --
 *     Atomically read a boolean.
 */
static inline bool
__wt_atomic_loadbool(bool *vp)
{
    return (__atomic_load_n(vp, __ATOMIC_RELAXED));
}

/*
 * __wt_atomic_storebool --
 *     Atomically set a boolean.
 */
static inline void
__wt_atomic_storebool(bool *vp, bool v)
{
    __atomic_store_n(vp, v, __ATOMIC_RELAXED);
}

/*
 * __wt_atomic_loadvbool --
 *     Atomically read a volatile boolean.
 */
static inline bool
__wt_atomic_loadvbool(volatile bool *vp)
{
    return (__atomic_load_n(vp, __ATOMIC_RELAXED));
}

/*
 * __wt_atomic_storevbool --
 *     Atomically set a volatile boolean.
 */
static inline void
__wt_atomic_storevbool(volatile bool *vp, bool v)
{
    __atomic_store_n(vp, v, __ATOMIC_RELAXED);
}

/*
 * Generic atomic functions that accept any type. The typed macros above should be preferred since
 * they provide better type checking.
 */
#define __wt_atomic_load_enum(vp) __atomic_load_n(vp, __ATOMIC_RELAXED)
#define __wt_atomic_store_enum(vp, v) __atomic_store_n(vp, v, __ATOMIC_RELAXED)
#define __wt_atomic_and_generic(vp, v) __atomic_and_fetch(vp, v, __ATOMIC_RELAXED)
#define __wt_atomic_or_generic(vp, v) __atomic_or_fetch(vp, v, __ATOMIC_RELAXED)
#define __wt_atomic_load_generic(vp) __atomic_load_n(vp, __ATOMIC_RELAXED)
#define __wt_atomic_store_generic(vp, v) __atomic_store_n(vp, v, __ATOMIC_RELAXED)

/*
 * These pointer specific macros behave identically to the generic ones above, but better
 * communicate intent and should be preferred over generic.
 */
#define __wt_atomic_load_pointer(vp) __wt_atomic_load_generic(vp)
#define __wt_atomic_store_pointer(vp, v) __wt_atomic_store_generic(vp, v)

/* Compile read-write barrier */
#define WT_COMPILER_BARRIER() __asm__ volatile("" ::: "memory")

#if defined(x86_64) || defined(__x86_64__)
/* Pause instruction to prevent excess processor bus usage */
#define WT_PAUSE() __asm__ volatile("pause\n" ::: "memory")
#define WT_FULL_BARRIER()                        \
    do {                                         \
        __asm__ volatile("mfence" ::: "memory"); \
    } while (0)
/* We only need compiler barriers on x86 due to Total Store Ordering (TSO). */
#define WT_ACQUIRE_BARRIER() WT_COMPILER_BARRIER()
#define WT_RELEASE_BARRIER() WT_COMPILER_BARRIER()

#elif defined(__mips64el__) || defined(__mips__) || defined(__mips64__) || defined(__mips64)
#define WT_PAUSE() __asm__ volatile("pause\n" ::: "memory")
#define WT_FULL_BARRIER()                                                                  \
    do {                                                                                   \
        __asm__ volatile("sync; ld $0, %0" ::"m"(*(long *)0xffffffff80000000) : "memory"); \
    } while (0)
#define WT_ACQUIRE_BARRIER() WT_FULL_BARRIER()
#define WT_RELEASE_BARRIER() WT_FULL_BARRIER()

#elif defined(__PPC64__) || defined(PPC64)
/* ori 0,0,0 is the PPC64 noop instruction */
#define WT_PAUSE() __asm__ volatile("ori 0,0,0" ::: "memory")
#define WT_FULL_BARRIER()                      \
    do {                                       \
        __asm__ volatile("sync" ::: "memory"); \
    } while (0)

/*
 * TODO: ISA 2.07 Elemental Memory Barriers would be better, specifically mbll, and mbss, but they
 * are not supported by POWER 8.
 */
#define WT_ACQUIRE_BARRIER()                     \
    do {                                         \
        __asm__ volatile("lwsync" ::: "memory"); \
    } while (0)
#define WT_RELEASE_BARRIER()                     \
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
 * ARM offers three barrier types:
 *   isb - instruction synchronization barrier
 *   dmb - data memory barrier
 *   dsb - data synchronization barrier
 *
 * To implement memory barriers for WiredTiger, we need at-least the dmb. dmb are sufficient to
 * guarantee the ordering described above. We don't want to use dsbs because they provide a much
 * stronger guarantee of completion which isn't required. Additionally, dsbs synchronize other
 * system activities such as tlb and cache maintenance instructions which is not required in this
 * case.
 *
 * A shareability domain of inner-shareable is selected because all the entities participating in
 * the ordering requirements are CPUs and ordering with respect to other devices or memory-types
 * isn't required.
 */
#define WT_FULL_BARRIER()                         \
    do {                                          \
        __asm__ volatile("dmb ish" ::: "memory"); \
    } while (0)
#define WT_ACQUIRE_BARRIER()                        \
    do {                                            \
        __asm__ volatile("dmb ishld" ::: "memory"); \
    } while (0)
/*
 * In order to achieve release semantics we need two arm barrier instructions. Firstly dmb ishst
 * which gives us StoreStore, secondly a dmb ishld which gives us LoadLoad, LoadStore.
 *
 * This is sufficient for the release semantics which is StoreStore, LoadStore. We could issue a
 * single dmb ish here but that is more expensive because it adds a StoreLoad barrier which is the
 * most expensive reordering to prevent.
 */
#define WT_RELEASE_BARRIER() __asm__ volatile("dmb ishst; dmb ishld" ::: "memory");

#elif defined(__s390x__)
#define WT_PAUSE() __asm__ volatile("lr 0,0" ::: "memory")
#define WT_FULL_BARRIER()                            \
    do {                                             \
        __asm__ volatile("bcr 15,0\n" ::: "memory"); \
    } while (0)
#define WT_ACQUIRE_BARRIER() WT_FULL_BARRIER()
#define WT_RELEASE_BARRIER() WT_FULL_BARRIER()

#elif defined(__sparc__)
#define WT_PAUSE() __asm__ volatile("rd %%ccr, %%g0" ::: "memory")

#define WT_FULL_BARRIER()                                   \
    do {                                                    \
        __asm__ volatile("membar #StoreLoad" ::: "memory"); \
    } while (0)

/* On UltraSparc machines, TSO is used, and so there is no need for membar. */
#define WT_ACQUIRE_BARRIER() WT_COMPILER_BARRIER()
#define WT_RELEASE_BARRIER() WT_COMPILER_BARRIER()

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
 * sets.
 */
#define WT_FULL_BARRIER()                              \
    do {                                               \
        __asm__ volatile("fence rw, rw" ::: "memory"); \
    } while (0)
#define WT_ACQUIRE_BARRIER()                          \
    do {                                              \
        __asm__ volatile("fence r, rw" ::: "memory"); \
    } while (0)
#define WT_RELEASE_BARRIER()                          \
    do {                                              \
        __asm__ volatile("fence rw, w" ::: "memory"); \
    } while (0)

#elif defined(__loongarch64)
#define WT_PAUSE() __asm__ volatile("nop\n" ::: "memory")
#define WT_FULL_BARRIER()                        \
    do {                                         \
        __asm__ volatile("dbar 0" ::: "memory"); \
    } while (0)
#define WT_ACQUIRE_BARRIER() WT_FULL_BARRIER()
#define WT_RELEASE_BARRIER() WT_FULL_BARRIER()
#else
#error "No barrier implementation for this hardware"
#endif

/*
 * WT_ACQUIRE_READ --
 *
 * The below assembly implements the read-acquire semantic. Acquire semantics prevent memory
 * reordering of the read-acquire with any load or store that follows it in program order.
 *
 * The if branches get removed at compile time as the sizeof instruction evaluates at compile time.
 * The inline assembly results in a loss of type checking, to circumvent this we utilize an
 * unreachable if (0) block which contains the direct assignment. This forces the compiler to type
 * check. We also statically assert that both types match in size to avoid potential loss of sign
 * when loading from a smaller type to a larger type.
 *
 * Depending on the size of the given type we choose the appropriate ldapr variant, additionally the
 * W register variants are used if possible which map to the lower word of the associated X
 * register. Finally the "Q" constraint is used for the given input operand, this instructs the
 * compiler to generate offset free ldapr instructions. ldapr instructions, prior to version RCpc 3,
 * don't support offsets.
 *
 * The flag HAVE_RCPC is determined by the build system, if this macro is removed in the future be
 * sure to remove that part of the compilation.
 */
#if defined(HAVE_RCPC) && !defined(TSAN_BUILD)
#define WT_ACQUIRE_READ(v, val)                                                                    \
    do {                                                                                           \
        if (0) {                                                                                   \
            static_assert(sizeof((v)) == sizeof((val)), "sizes of provided variables must match"); \
            (v) = (val);                                                                           \
        }                                                                                          \
        if (sizeof((val)) == 1) {                                                                  \
            __asm__ volatile("ldaprb %w0, %1" : "=r"(v) : "Q"(val));                               \
        } else if (sizeof((val)) == 2) {                                                           \
            __asm__ volatile("ldaprh %w0, %1" : "=r"(v) : "Q"(val));                               \
        } else if (sizeof((val)) == 4) {                                                           \
            __asm__ volatile("ldapr %w0, %1" : "=r"(v) : "Q"(val));                                \
        } else if (sizeof((val)) == 8) {                                                           \
            __asm__ volatile("ldapr %x0, %1" : "=r"(v) : "Q"(val));                                \
        }                                                                                          \
    } while (0)
#else
#define WT_ACQUIRE_READ(v, val) (v) = __atomic_load_n(&(val), __ATOMIC_ACQUIRE)
#endif

/*
 * WT_RELEASE_WRITE --
 *
 * Write to a memory location using the ARM stlr instruction. This is also known as a write-release
 * operation, and has the following semantics: Release semantics prevent memory reordering of the
 * write-release with any read or write operation that precedes it in program order.
 *
 * Usage of this macro should be paired with an associated WT_ACQUIRE_READ. As with the acquire
 * version we avoid type checking loss by defining an unreachable if block, we also guard against
 * misuse by statically asserting that the destination is the same size as the value being written.
 */
#if defined(HAVE_RCPC) && !defined(TSAN_BUILD)
#define WT_RELEASE_WRITE(v, val)                                                                   \
    do {                                                                                           \
        if (0) {                                                                                   \
            static_assert(sizeof((v)) == sizeof((val)), "sizes of provided variables must match"); \
            (v) = (val);                                                                           \
        }                                                                                          \
        if (sizeof((v)) == 1) {                                                                    \
            __asm__ volatile("stlrb %w1, %0" : "=Q"(v) : "r"(val));                                \
        } else if (sizeof((v)) == 2) {                                                             \
            __asm__ volatile("stlrh %w1, %0" : "=Q"(v) : "r"(val));                                \
        } else if (sizeof((v)) == 4) {                                                             \
            __asm__ volatile("stlr %w1, %0" : "=Q"(v) : "r"(val));                                 \
        } else if (sizeof((v)) == 8) {                                                             \
            __asm__ volatile("stlr %x1, %0" : "=Q"(v) : "r"(val));                                 \
        }                                                                                          \
    } while (0)
#else
#define WT_RELEASE_WRITE(v, val) __atomic_store_n(&(v), (val), __ATOMIC_RELEASE)
#endif
