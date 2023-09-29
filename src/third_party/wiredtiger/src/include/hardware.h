/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Publish a value to a shared location. All previous stores must complete before the value is made
 * public.
 */
#define WT_PUBLISH(v, val)  \
    do {                    \
        WT_WRITE_BARRIER(); \
        (v) = (val);        \
    } while (0)

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
 * inserting a read barrier after the read (our pre-existing implementation) which prevents invented
 * and fused loads for this variable in the code following the expression.
 *
 * FIXME-WT-11718 - Once Windows build machines that support C11 _Generics are available this macro
 * will be updated to use _Generic on all platforms.
 */
#if defined(__GNUC__) || defined(__clang__)
#define WT_READ_ONCE(v, val) (v) = (*(volatile __typeof__(val) *)&(val))
#else
#define WT_READ_ONCE(v, val) WT_ORDERED_READ(v, val)
#endif

/*
 * Read a shared location and guarantee that subsequent reads do not see any earlier state.
 */
#define WT_ORDERED_READ(v, val) \
    do {                        \
        (v) = (val);            \
        WT_READ_BARRIER();      \
    } while (0)

/*
 * In some architectures with weak memory ordering, the CPU can reorder the reads across full
 * barriers in other threads. Guarantee that subsequent reads do not see any earlier state in those
 * architectures.
 *
 * !!! This is a temporary solution to avoid a performance regression in x86. Do not use this macro
 * and we will revisit it later.
 */
#define WT_ORDERED_READ_WEAK_MEMORDER(v, val) \
    do {                                      \
        (v) = (val);                          \
        WT_READ_BARRIER_WEAK_MEMORDER();      \
    } while (0)

/*
 * Atomic versions of the flag set/clear macros.
 */
#define FLD_ISSET_ATOMIC_16(field, mask) ((field) & (uint16_t)(mask))

#define FLD_SET_ATOMIC_16(field, mask)                                             \
    do {                                                                           \
        uint16_t __orig;                                                           \
        if (FLD_ISSET_ATOMIC_16((field), (mask)))                                  \
            break;                                                                 \
        do {                                                                       \
            __orig = (field);                                                      \
        } while (!__wt_atomic_cas16(&(field), __orig, __orig | (uint16_t)(mask))); \
    } while (0)

#define FLD_CLR_ATOMIC_16(field, mask)                                                \
    do {                                                                              \
        uint16_t __orig;                                                              \
        if (!FLD_ISSET_ATOMIC_16((field), (mask)))                                    \
            break;                                                                    \
        do {                                                                          \
            __orig = (field);                                                         \
        } while (!__wt_atomic_cas16(&(field), __orig, __orig & (uint16_t)(~(mask)))); \
    } while (0)

#define F_ISSET_ATOMIC_16(p, mask) FLD_ISSET_ATOMIC_16((p)->flags_atomic, mask)
#define F_CLR_ATOMIC_16(p, mask) FLD_CLR_ATOMIC_16((p)->flags_atomic, mask)
#define F_SET_ATOMIC_16(p, mask) FLD_SET_ATOMIC_16((p)->flags_atomic, mask)

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
