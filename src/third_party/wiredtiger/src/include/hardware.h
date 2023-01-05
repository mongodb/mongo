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
 * Read a shared location and guarantee that subsequent reads do not see any earlier state.
 */
#define WT_ORDERED_READ(v, val) \
    do {                        \
        (v) = (val);            \
        WT_READ_BARRIER();      \
    } while (0)

/*
 * Atomic versions of the flag set/clear macros.
 */

#define F_ISSET_ATOMIC_16(p, mask) ((p)->flags_atomic & (uint16_t)(mask))

#define F_SET_ATOMIC_16(p, mask)                                                             \
    do {                                                                                     \
        uint16_t __orig;                                                                     \
        if (F_ISSET_ATOMIC_16(p, mask))                                                      \
            break;                                                                           \
        do {                                                                                 \
            __orig = (p)->flags_atomic;                                                      \
        } while (!__wt_atomic_cas16(&(p)->flags_atomic, __orig, __orig | (uint16_t)(mask))); \
    } while (0)

#define F_CLR_ATOMIC_16(p, mask)                                                                \
    do {                                                                                        \
        uint16_t __orig;                                                                        \
        if (!F_ISSET_ATOMIC_16(p, mask))                                                        \
            break;                                                                              \
        do {                                                                                    \
            __orig = (p)->flags_atomic;                                                         \
        } while (!__wt_atomic_cas16(&(p)->flags_atomic, __orig, __orig & (uint16_t)(~(mask)))); \
    } while (0)

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
