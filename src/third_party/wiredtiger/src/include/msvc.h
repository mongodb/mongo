/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#include <intrin.h>

#ifndef _M_AMD64
#error "Only x64 is supported with MSVC"
#endif

#ifndef __cplusplus
#define inline __inline
#endif

/* MSVC Doesn't provide __PRETTY_FUNCTION__, it has __FUNCSIG__ */
#ifdef _MSC_VER
#define __PRETTY_FUNCTION__ __FUNCSIG__
#endif

#define WT_PTRDIFFT_FMT "Id" /* ptrdiff_t format string */
#define WT_SIZET_FMT "Iu"    /* size_t format string */

/* MSVC-specific attributes. */
#define WT_PACKED_STRUCT_BEGIN(name) __pragma(pack(push, 1)) struct name {

#define WT_PACKED_STRUCT_END \
    }                        \
    ;                        \
    __pragma(pack(pop))

#define WT_GCC_FUNC_ATTRIBUTE(x)
#define WT_GCC_FUNC_DECL_ATTRIBUTE(x)

#define WT_ATOMIC_FUNC(name, ret, type, s, t)                                                     \
    static inline ret __wt_atomic_add##name(type *vp, type v)                                     \
    {                                                                                             \
        return (_InterlockedExchangeAdd##s((t *)(vp), (t)(v)) + (v));                             \
    }                                                                                             \
    static inline ret __wt_atomic_fetch_add##name(type *vp, type v)                               \
    {                                                                                             \
        return (_InterlockedExchangeAdd##s((t *)(vp), (t)(v)));                                   \
    }                                                                                             \
    static inline ret __wt_atomic_sub##name(type *vp, type v)                                     \
    {                                                                                             \
        return (_InterlockedExchangeAdd##s((t *)(vp), -(t)v) - (v));                              \
    }                                                                                             \
    static inline bool __wt_atomic_cas##name(type *vp, type old_val, type new_val)                \
    {                                                                                             \
        return (                                                                                  \
          _InterlockedCompareExchange##s((t *)(vp), (t)(new_val), (t)(old_val)) == (t)(old_val)); \
    }

WT_ATOMIC_FUNC(8, uint8_t, uint8_t, 8, char)
WT_ATOMIC_FUNC(v8, uint8_t, volatile uint8_t, 8, char)
WT_ATOMIC_FUNC(16, uint16_t, uint16_t, 16, short)
WT_ATOMIC_FUNC(32, uint32_t, uint32_t, , long)
WT_ATOMIC_FUNC(v32, uint32_t, volatile uint32_t, , long)
WT_ATOMIC_FUNC(i32, int32_t, int32_t, , long)
WT_ATOMIC_FUNC(iv32, int32_t, volatile int32_t, , long)
WT_ATOMIC_FUNC(64, uint64_t, uint64_t, 64, __int64)
WT_ATOMIC_FUNC(v64, uint64_t, volatile uint64_t, 64, __int64)
WT_ATOMIC_FUNC(i64, int64_t, int64_t, 64, __int64)
WT_ATOMIC_FUNC(iv64, int64_t, volatile int64_t, 64, __int64)
WT_ATOMIC_FUNC(size, size_t, size_t, 64, __int64)

/*
 * __wt_atomic_cas_ptr --
 *     Pointer compare and swap.
 */
static inline bool
__wt_atomic_cas_ptr(void *vp, void *old_val, void *new_val)
{
    return (_InterlockedCompareExchange64(
              (volatile __int64 *)vp, (int64_t)new_val, (int64_t)old_val) == ((int64_t)old_val));
}

/*
 * WT_BARRIER --
 *	MSVC implementation of WT_BARRIER.
 */
static inline void
WT_BARRIER(void)
{
    _ReadWriteBarrier();
}

/*
 * WT_FULL_BARRIER --
 *	MSVC implementation of WT_FULL_BARRIER.
 */
static inline void
WT_FULL_BARRIER(void)
{
    _mm_mfence();
}

/*
 * WT_PAUSE --
 *	MSVC implementation of WT_PAUSE.
 */
static inline void
WT_PAUSE(void)
{
    _mm_pause();
}

/*
 * WT_READ_BARRIER --
 *	MSVC implementation of WT_READ_BARRIER.
 */
static inline void
WT_READ_BARRIER(void)
{
    _mm_lfence();
}

/*
 * WT_READ_BARRIER_WEAK_MEMORDER --
 *	MSVC implementation of WT_READ_BARRIER_WEAK_MEMORDER.
 */
static inline void
WT_READ_BARRIER_WEAK_MEMORDER(void)
{
    /* x86 has a strong memory model, so we only need a compiler barrier here. */
#ifdef _M_AMD64
    WT_BARRIER();
#else
    /* Default to a stronger read barrier for other platforms. */
    WT_READ_BARRIER();
#endif
}

/*
 * WT_WRITE_BARRIER --
 *	MSVC implementation of WT_WRITE_BARRIER.
 */
static inline void
WT_WRITE_BARRIER(void)
{
    _mm_sfence();
}
