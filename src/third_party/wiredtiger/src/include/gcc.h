/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#include <assert.h>

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
 * This macro is for internal use within this document only. For all other cases, please use
 * __wt_atomic_load_<type>_acquire(...)
 *
 * The below assembly implements the read-acquire semantic. Acquire semantics prevent memory
 * reordering of the read-acquire with any load or store that follows it in program order.
 *
 * The if branches get removed at compile time as the sizeof instruction evaluates at compile time.
 * The inline assembly results in a loss of type checking, to circumvent this we utilize an
 * unreachable if (0) block which contains the direct assignment. This forces the compiler to
 * type check. We also statically assert that both types match in size to avoid potential loss
 * of sign when loading from a smaller type to a larger type.
 *
 * Depending on the size of the given type we choose the appropriate ldapr variant, additionally the
 * W register variants are used if possible which map to the lower word of the associated X
 * register. Finally the "Q" constraint is used for the given input operand, this instructs the
 * compiler to generate offset free ldapr instructions. ldapr instructions, prior to version
 * RCpc 3, don't support offsets.
 *
 * The flag HAVE_RCPC is determined by the build system, if this macro is removed in the future be
 * sure to remove that part of the compilation.
 */
#if defined(HAVE_RCPC) && !defined(TSAN_BUILD)
#define ACQUIRE_READ(v, val)                                                                       \
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
#define ACQUIRE_READ(v, val) (v) = __atomic_load_n(&(val), __ATOMIC_ACQUIRE)
#endif

/*
 * This macro is for internal use within this document only. For all other cases, please use
 * __wt_atomic_store_<type>_release(...)
 *
 * Write to a memory location using the ARM stlr instruction. This is also known as a write-release
 * operation, and has the following semantics: Release semantics prevent memory reordering of
 * the write-release with any read or write operation that precedes it in program order.
 *
 * Usage of this macro should be paired with an associated WT_ACQUIRE_READ. As with the acquire
 * version we avoid type checking loss by defining an unreachable if block, we also guard
 * against misuse by statically asserting that the destination is the same size as the value
 * being written.
 */
#if defined(HAVE_RCPC) && !defined(TSAN_BUILD)
#define RELEASE_WRITE(v, val)                                                                      \
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
#define RELEASE_WRITE(v, val) __atomic_store_n(&(v), (val), __ATOMIC_RELEASE)
#endif

/*
 * This macro is for internal use within this document only. For all other cases, please use
 * __wt_atomic_cas_<type>(...)
 */
#define ATOMIC_CAS(ptr, oldp, newv) \
    __atomic_compare_exchange_n(ptr, oldp, newv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

#define WT_ATOMIC_FUNC_STORE_LOAD(suffix, _type)                                           \
    static inline _type __wt_atomic_load_##suffix##_relaxed(_type *vp)                     \
    {                                                                                      \
        return (__atomic_load_n(vp, __ATOMIC_RELAXED));                                    \
    }                                                                                      \
    static inline void __wt_atomic_store_##suffix##_relaxed(_type *vp, _type v)            \
    {                                                                                      \
        __atomic_store_n(vp, v, __ATOMIC_RELAXED);                                         \
    }                                                                                      \
    static inline _type __wt_atomic_load_##suffix##_acquire(_type *vp)                     \
    {                                                                                      \
        _type result;                                                                      \
        ACQUIRE_READ(result, *(vp));                                                       \
        return (result);                                                                   \
    }                                                                                      \
    static inline void __wt_atomic_store_##suffix##_release(_type *vp, _type v)            \
    {                                                                                      \
        RELEASE_WRITE(*(vp), v);                                                           \
    }                                                                                      \
    static inline _type __wt_atomic_load_##suffix##_v_relaxed(volatile _type *vp)          \
    {                                                                                      \
        return (__atomic_load_n(vp, __ATOMIC_RELAXED));                                    \
    }                                                                                      \
    static inline void __wt_atomic_store_##suffix##_v_relaxed(volatile _type *vp, _type v) \
    {                                                                                      \
        __atomic_store_n(vp, v, __ATOMIC_RELAXED);                                         \
    }                                                                                      \
    static inline _type __wt_atomic_load_##suffix##_v_acquire(volatile _type *vp)          \
    {                                                                                      \
        _type result;                                                                      \
        ACQUIRE_READ(result, *(vp));                                                       \
        return (result);                                                                   \
    }                                                                                      \
    static inline void __wt_atomic_store_##suffix##_v_release(volatile _type *vp, _type v) \
    {                                                                                      \
        RELEASE_WRITE(*(vp), v);                                                           \
    }

#define WT_ATOMIC_CAS_FUNC(suffix, _type)                                                      \
    static inline bool __wt_atomic_cas_##suffix(_type *vp, _type old, _type newv)              \
    {                                                                                          \
        return (ATOMIC_CAS(vp, &old, newv));                                                   \
    }                                                                                          \
    static inline bool __wt_atomic_cas_##suffix##_v(volatile _type *vp, _type old, _type newv) \
    {                                                                                          \
        return (ATOMIC_CAS(vp, &old, newv));                                                   \
    }

#define WT_ATOMIC_FUNC(suffix, _type)                                                   \
    static inline _type __wt_atomic_add_##suffix(_type *vp, _type v)                    \
    {                                                                                   \
        return (__atomic_add_fetch(vp, v, __ATOMIC_SEQ_CST));                           \
    }                                                                                   \
    static inline _type __wt_atomic_add_##suffix##_relaxed(_type *vp, _type v)          \
    {                                                                                   \
        return (__atomic_add_fetch(vp, v, __ATOMIC_RELAXED));                           \
    }                                                                                   \
    static inline _type __wt_atomic_fetch_add_##suffix(_type *vp, _type v)              \
    {                                                                                   \
        return (__atomic_fetch_add(vp, v, __ATOMIC_SEQ_CST));                           \
    }                                                                                   \
    static inline _type __wt_atomic_sub_##suffix(_type *vp, _type v)                    \
    {                                                                                   \
        return (__atomic_sub_fetch(vp, v, __ATOMIC_SEQ_CST));                           \
    }                                                                                   \
    static inline _type __wt_atomic_sub_##suffix##_relaxed(_type *vp, _type v)          \
    {                                                                                   \
        return (__atomic_sub_fetch(vp, v, __ATOMIC_RELAXED));                           \
    }                                                                                   \
    static inline _type __wt_atomic_add_##suffix##_v(volatile _type *vp, _type v)       \
    {                                                                                   \
        return (__atomic_add_fetch(vp, v, __ATOMIC_SEQ_CST));                           \
    }                                                                                   \
    static inline _type __wt_atomic_fetch_add_##suffix##_v(volatile _type *vp, _type v) \
    {                                                                                   \
        return (__atomic_fetch_add(vp, v, __ATOMIC_SEQ_CST));                           \
    }                                                                                   \
    static inline _type __wt_atomic_sub_##suffix##_v(volatile _type *vp, _type v)       \
    {                                                                                   \
        return (__atomic_sub_fetch(vp, v, __ATOMIC_SEQ_CST));                           \
    }                                                                                   \
    WT_ATOMIC_CAS_FUNC(suffix, _type)                                                   \
    WT_ATOMIC_FUNC_STORE_LOAD(suffix, _type)

WT_ATOMIC_FUNC(uint8, uint8_t)
WT_ATOMIC_FUNC(uint16, uint16_t)
WT_ATOMIC_FUNC(uint32, uint32_t)
WT_ATOMIC_FUNC(uint64, uint64_t)
WT_ATOMIC_FUNC(int8, int8_t)
WT_ATOMIC_FUNC(int16, int16_t)
WT_ATOMIC_FUNC(int32, int32_t)
WT_ATOMIC_FUNC(int64, int64_t)
WT_ATOMIC_FUNC(size, size_t)

WT_ATOMIC_FUNC_STORE_LOAD(bool, bool)

/*
 * __wt_atomic_load_double_relaxed --
 *     Atomically read a double variable.
 */
static inline double
__wt_atomic_load_double_relaxed(double *vp)
{
    double value;
    __atomic_load(vp, &value, __ATOMIC_RELAXED);
    return (value);
}

/*
 * __wt_atomic_store_double_relaxed --
 *     Atomically set a double variable.
 */
static inline void
__wt_atomic_store_double_relaxed(double *vp, double v)
{
    __atomic_store(vp, &v, __ATOMIC_RELAXED);
}

#define __wt_atomic_load_enum_relaxed(vp) __atomic_load_n(vp, __ATOMIC_RELAXED)
#define __wt_atomic_store_enum_relaxed(vp, v) __atomic_store_n(vp, v, __ATOMIC_RELAXED)

#define __wt_atomic_load_ptr_relaxed(vp) __atomic_load_n(vp, __ATOMIC_RELAXED)
#define __wt_atomic_store_ptr_relaxed(vp, v) __atomic_store_n(vp, v, __ATOMIC_RELAXED)
#define __wt_atomic_load_ptr_acquire(vp) __atomic_load_n(vp, __ATOMIC_ACQUIRE)
#define __wt_atomic_store_ptr_release(vp, v) __atomic_store_n(vp, v, __ATOMIC_RELEASE)

/*
 * __wt_atomic_cas_ptr --
 *     Pointer compare and swap.
 */
static inline bool
__wt_atomic_cas_ptr(void *vp, void *old, void *newv)
{
    return (ATOMIC_CAS((void **)vp, &old, newv));
}

#define __wt_atomic_and_generic_relaxed(vp, v) __atomic_and_fetch(vp, v, __ATOMIC_RELAXED)
#define __wt_atomic_or_generic_relaxed(vp, v) __atomic_or_fetch(vp, v, __ATOMIC_RELAXED)
#define __wt_atomic_load_generic_relaxed(vp) __atomic_load_n(vp, __ATOMIC_RELAXED)
#define __wt_atomic_store_generic_relaxed(vp, v) __atomic_store_n(vp, v, __ATOMIC_RELAXED)
