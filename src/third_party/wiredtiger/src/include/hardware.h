/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * This macro doesn't do anything and is used for annotation only. We use it to highlight
 * the variable is used in lock-less inter-thread communication - using mechanisms like memory
 * barriers and compare_and_swap - and requires caution when handling. It is designed to look like a
 * type qualifier.
 *
 * Example usage:
 *     wt_shared volatile bool blkcache_exiting;
 */
#define wt_shared

/*
 * Deprecated: use WT_RELEASE_WRITE instead. Release write a value to a shared location. All
 * previous stores must complete before the value is made public.
 *
 * Use atomic based operations for TSAN since it doesn't track standalone barriers.
 */
#if defined(TSAN_BUILD)
#define WT_RELEASE_WRITE_WITH_BARRIER(v, val) __atomic_store_n(&(v), (val), __ATOMIC_RELEASE)
#else
#define WT_RELEASE_WRITE_WITH_BARRIER(v, val)   \
    do {                                        \
        WT_RELEASE_BARRIER();                   \
        __wt_atomic_store_generic(&(v), (val)); \
    } while (0)
#endif

/*
 * WT_READ_ONCE --
 *
 * Ensure a single read from memory in the source code produces a single read from memory in the
 * compiled output.
 *
 * The compiler is allowed to optimize loads from memory in multiple ways such as "load fusing",
 * where the compiler takes multiple reads from a memory location and merges them into a single read
 * instruction, and "invented loads" where the compiler takes a single load from memory and converts
 * it into multiple read instructions.
 *
 * WiredTiger has many algorithms where threads are allowed to concurrently access and modify the
 * same memory location, but to do this safely we need to precisely control how reads to memory are
 * performed. This macro gives us control over this.
 *
 * GCC and Clang have a __typeof__ compiler builtin which allows us to temporarily cast the value
 * being read as a volatile and achieve volatile semantics. For other compilers we'll fall back on
 * inserting an acquire barrier after the read (our pre-existing implementation) which prevents
 * invented and fused loads for this variable in the code following the expression.
 */
#if defined(__GNUC__) || defined(__clang__)
#define WT_READ_ONCE(v, val) (v) = __wt_atomic_load_generic((volatile __typeof__(val) *)&(val))
#else
#define WT_READ_ONCE(v, val) WT_ACQUIRE_READ_WITH_BARRIER(v, val)
#endif

/*
 * WT_WRITE_ONCE --
 *
 * Ensure a single write to memory in the source code produces a single write to memory in the
 * compiled output.
 *
 * See the read once macro description for more details.
 */
#if defined(__GNUC__) || defined(__clang__)
#define WT_WRITE_ONCE(v, val) __wt_atomic_store_generic((volatile __typeof__(v) *)&(v), (val))
#else
#define WT_WRITE_ONCE(v, val) WT_RELEASE_WRITE_WITH_BARRIER(v, val)
#endif

/*
 * Deprecated: use WT_ACQUIRE_READ instead. Read a shared location and guarantee that subsequent
 * reads do not see any earlier state.
 *
 * Use atomic based operations for TSAN since it doesn't track standalone barriers.
 */
#if defined(TSAN_BUILD)
#define WT_ACQUIRE_READ_WITH_BARRIER(v, val) (v) = __atomic_load_n(&(val), __ATOMIC_ACQUIRE)
#else
#define WT_ACQUIRE_READ_WITH_BARRIER(v, val)    \
    do {                                        \
        (v) = __wt_atomic_load_generic(&(val)); \
        WT_ACQUIRE_BARRIER();                   \
    } while (0)
#endif

/*
 * Atomic versions of the flag set/clear macros.
 */
#define FLD_ISSET_ATOMIC_8(field, mask) (__wt_atomic_load8(&(field)) & (uint8_t)(mask))

#define FLD_ISSET_ATOMIC_16(field, mask) (__wt_atomic_load16(&(field)) & (uint16_t)(mask))

#define FLD_ISSET_ATOMIC_32(field, mask) (__wt_atomic_load32(&(field)) & (uint32_t)(mask))

#define FLD_SET_ATOMIC_8(field, mask)                                            \
    do {                                                                         \
        uint8_t __orig;                                                          \
        if (FLD_ISSET_ATOMIC_8((field), (mask)))                                 \
            break;                                                               \
        do {                                                                     \
            __orig = __wt_atomic_load8(&(field));                                \
        } while (!__wt_atomic_cas8(&(field), __orig, __orig | (uint8_t)(mask))); \
    } while (0)

#define FLD_SET_ATOMIC_16(field, mask)                                             \
    do {                                                                           \
        uint16_t __orig;                                                           \
        if (FLD_ISSET_ATOMIC_16((field), (mask)))                                  \
            break;                                                                 \
        do {                                                                       \
            __orig = __wt_atomic_load16(&(field));                                 \
        } while (!__wt_atomic_cas16(&(field), __orig, __orig | (uint16_t)(mask))); \
    } while (0)

#define FLD_SET_ATOMIC_32(field, mask)                                             \
    do {                                                                           \
        uint32_t __orig;                                                           \
        if (FLD_ISSET_ATOMIC_32((field), (mask)))                                  \
            break;                                                                 \
        do {                                                                       \
            __orig = __wt_atomic_load32(&(field));                                 \
        } while (!__wt_atomic_cas32(&(field), __orig, __orig | (uint32_t)(mask))); \
    } while (0)

#define FLD_CLR_ATOMIC_8(field, mask)                                               \
    do {                                                                            \
        uint8_t __orig;                                                             \
        if (!FLD_ISSET_ATOMIC_8((field), (mask)))                                   \
            break;                                                                  \
        do {                                                                        \
            __orig = __wt_atomic_load8(&(field));                                   \
        } while (!__wt_atomic_cas8(&(field), __orig, __orig & (uint8_t)(~(mask)))); \
    } while (0)

#define FLD_CLR_ATOMIC_16(field, mask)                                                \
    do {                                                                              \
        uint16_t __orig;                                                              \
        if (!FLD_ISSET_ATOMIC_16((field), (mask)))                                    \
            break;                                                                    \
        do {                                                                          \
            __orig = __wt_atomic_load16(&(field));                                    \
        } while (!__wt_atomic_cas16(&(field), __orig, __orig & (uint16_t)(~(mask)))); \
    } while (0)

#define FLD_CLR_ATOMIC_32(field, mask)                                                \
    do {                                                                              \
        uint32_t __orig;                                                              \
        if (!FLD_ISSET_ATOMIC_32((field), (mask)))                                    \
            break;                                                                    \
        do {                                                                          \
            __orig = __wt_atomic_load32(&(field));                                    \
        } while (!__wt_atomic_cas32(&(field), __orig, __orig & (uint32_t)(~(mask)))); \
    } while (0)

#define F_ISSET_ATOMIC_8(p, mask) FLD_ISSET_ATOMIC_8((p)->flags_atomic, mask)
#define F_ISSET_ATOMIC_16(p, mask) FLD_ISSET_ATOMIC_16((p)->flags_atomic, mask)
#define F_ISSET_ATOMIC_32(p, mask) FLD_ISSET_ATOMIC_32((p)->flags_atomic, mask)
#define F_CLR_ATOMIC_8(p, mask) FLD_CLR_ATOMIC_8((p)->flags_atomic, mask)
#define F_CLR_ATOMIC_16(p, mask) FLD_CLR_ATOMIC_16((p)->flags_atomic, mask)
#define F_CLR_ATOMIC_32(p, mask) FLD_CLR_ATOMIC_32((p)->flags_atomic, mask)
#define F_SET_ATOMIC_8(p, mask) FLD_SET_ATOMIC_8((p)->flags_atomic, mask)
#define F_SET_ATOMIC_16(p, mask) FLD_SET_ATOMIC_16((p)->flags_atomic, mask)
#define F_SET_ATOMIC_32(p, mask) FLD_SET_ATOMIC_32((p)->flags_atomic, mask)

/*
 * Cache line alignment.
 */
#if defined(__PPC64__) || defined(PPC64)
#define WT_CACHE_LINE_ALIGNMENT 128
#elif defined(__s390x__)
#define WT_CACHE_LINE_ALIGNMENT 256
#else
#define WT_CACHE_LINE_ALIGNMENT 64
#endif

/*
 * Pad a structure so an array of structures get separate cache lines.
 *
 * Note that we avoid compiler structure alignment because that requires allocating aligned blocks
 * of memory, and alignment pollutes any other type that contains an aligned field. It is possible
 * that a hot field positioned before this one will be on the same cache line, but not if it is also
 * padded.
 *
 * This alignment has a small impact on portability as well, as we are using an anonymous union here
 * which is supported under C11, earlier versions of the GNU standard, and MSVC versions as early as
 * 2003.
 */
#define WT_CACHE_LINE_PAD_BEGIN \
    union {                     \
        struct {
#define WT_CACHE_LINE_PAD_END                \
    }                                        \
    ;                                        \
    char __padding[WT_CACHE_LINE_ALIGNMENT]; \
    }                                        \
    ;
