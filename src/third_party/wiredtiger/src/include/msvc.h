/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

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

/*
 * WT_COMPILER_BARRIER --
 *	MSVC implementation of WT_COMPILER_BARRIER.
 */
static inline void
WT_COMPILER_BARRIER(void)
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
 * WT_ACQUIRE_BARRIER --
 *	MSVC implementation of WT_ACQUIRE_BARRIER. As we're running on x86 TSO we only issue a
 *compiler barrier.
 */
static inline void
WT_ACQUIRE_BARRIER(void)
{
    WT_COMPILER_BARRIER();
}

/*
 * WT_RELEASE_BARRIER --
 *	MSVC implementation of WT_RELEASE_BARRIER. As we're running on x86 TSO we only issue a
 *compiler barrier.
 */
static inline void
WT_RELEASE_BARRIER(void)
{
    WT_COMPILER_BARRIER();
}

/*
 * !!!
 * The following functions do not use atomic accesses like they do in gcc.h. MSVC doesn't have
 * the equivalent relaxed memory ordering atomics on x86 (only ARM has Interlocked*_nf
 * functions that don't output a fence), so use non-atomic accesses which was the behavior
 * prior the addition of atomic load and store.
 */

#define WT_ATOMIC_FUNC_STORE_LOAD(suffix, _type)                                           \
    static inline _type __wt_atomic_load_##suffix##_relaxed(_type *vp)                     \
    {                                                                                      \
        return (*(vp));                                                                    \
    }                                                                                      \
    static inline void __wt_atomic_store_##suffix##_relaxed(_type *vp, _type v)            \
    {                                                                                      \
        *(vp) = (v);                                                                       \
    }                                                                                      \
    static inline _type __wt_atomic_load_##suffix##_acquire(_type *vp)                     \
    {                                                                                      \
        _type result = *vp;                                                                \
        WT_ACQUIRE_BARRIER();                                                              \
        return (result);                                                                   \
    }                                                                                      \
    static inline void __wt_atomic_store_##suffix##_release(_type *vp, _type v)            \
    {                                                                                      \
        WT_RELEASE_BARRIER();                                                              \
        *vp = v;                                                                           \
    }                                                                                      \
    static inline _type __wt_atomic_load_##suffix##_v_relaxed(volatile _type *vp)          \
    {                                                                                      \
        return (*(vp));                                                                    \
    }                                                                                      \
    static inline void __wt_atomic_store_##suffix##_v_relaxed(volatile _type *vp, _type v) \
    {                                                                                      \
        *(vp) = (v);                                                                       \
    }                                                                                      \
    static inline _type __wt_atomic_load_##suffix##_v_acquire(volatile _type *vp)          \
    {                                                                                      \
        _type result = *vp;                                                                \
        WT_ACQUIRE_BARRIER();                                                              \
        return (result);                                                                   \
    }                                                                                      \
    static inline void __wt_atomic_store_##suffix##_v_release(volatile _type *vp, _type v) \
    {                                                                                      \
        WT_RELEASE_BARRIER();                                                              \
        *vp = v;                                                                           \
    }

#define WT_ATOMIC_CAS_FUNC(suffix, _type, s, t)                                                \
    static inline bool __wt_atomic_cas_##suffix(_type *vp, _type old, _type newv)              \
    {                                                                                          \
        return (__WT_ATOMIC_CAS_INTERNAL(vp, &old, newv));                                     \
    }                                                                                          \
    static inline bool __wt_atomic_cas_##suffix##_v(volatile _type *vp, _type old, _type newv) \
    {                                                                                          \
        return (__WT_ATOMIC_CAS_INTERNAL(vp, &old, newv));                                     \
    }

#define WT_ATOMIC_FUNC(suffix, _type, s, t)                                                       \
    static inline _type __wt_atomic_add_##suffix(_type *vp, _type v)                              \
    {                                                                                             \
        return (_InterlockedExchangeAdd##s((t *)(vp), (t)(v)) + (v));                             \
    }                                                                                             \
    static inline _type __wt_atomic_add_##suffix##_relaxed(_type *vp, _type v)                    \
    {                                                                                             \
        return (_InterlockedExchangeAdd##s((t *)(vp), (t)(v)) + (v));                             \
    }                                                                                             \
    static inline _type __wt_atomic_fetch_add_##suffix(_type *vp, _type v)                        \
    {                                                                                             \
        return (_InterlockedExchangeAdd##s((t *)(vp), (t)(v)));                                   \
    }                                                                                             \
    static inline _type __wt_atomic_sub_##suffix(_type *vp, _type v)                              \
    {                                                                                             \
        return (_InterlockedExchangeAdd##s((t *)(vp), -(t)v) - (v));                              \
    }                                                                                             \
    static inline _type __wt_atomic_sub_##suffix##_relaxed(_type *vp, _type v)                    \
    {                                                                                             \
        return (_InterlockedExchangeAdd##s((t *)(vp), -(t)v) - (v));                              \
    }                                                                                             \
    static inline _type __wt_atomic_add_##suffix##_v(volatile _type *vp, _type v)                 \
    {                                                                                             \
        return (_InterlockedExchangeAdd##s((t *)(vp), (t)(v)) + (v));                             \
    }                                                                                             \
    static inline _type __wt_atomic_fetch_add_##suffix##_v(volatile _type *vp, _type v)           \
    {                                                                                             \
        return (_InterlockedExchangeAdd##s((t *)(vp), (t)(v)));                                   \
    }                                                                                             \
    static inline _type __wt_atomic_sub_##suffix##_v(volatile _type *vp, _type v)                 \
    {                                                                                             \
        return (_InterlockedExchangeAdd##s((t *)(vp), -(t)v) - (v));                              \
    }                                                                                             \
    static inline bool __wt_atomic_cas_##suffix##_v(                                              \
      volatile _type *vp, _type old_val, _type new_val)                                           \
    {                                                                                             \
        return (                                                                                  \
          _InterlockedCompareExchange##s((t *)(vp), (t)(new_val), (t)(old_val)) == (t)(old_val)); \
    }                                                                                             \
    static inline bool __wt_atomic_cas_##suffix(_type *vp, _type old_val, _type new_val)          \
    {                                                                                             \
        return (                                                                                  \
          _InterlockedCompareExchange##s((t *)(vp), (t)(new_val), (t)(old_val)) == (t)(old_val)); \
    }                                                                                             \
    WT_ATOMIC_FUNC_STORE_LOAD(suffix, _type)

WT_ATOMIC_FUNC(uint8, uint8_t, 8, char)
WT_ATOMIC_FUNC(uint16, uint16_t, 16, short)
WT_ATOMIC_FUNC(uint32, uint32_t, , long)
WT_ATOMIC_FUNC(uint64, uint64_t, 64, __int64)
WT_ATOMIC_FUNC(int8, int8_t, 8, char)
WT_ATOMIC_FUNC(int16, int16_t, 16, short)
WT_ATOMIC_FUNC(int32, int32_t, , long)
WT_ATOMIC_FUNC(int64, int64_t, 64, __int64)
WT_ATOMIC_FUNC(size, size_t, 64, __int64)

WT_ATOMIC_FUNC_STORE_LOAD(bool, bool)

/*
 * __wt_atomic_load_double_relaxed --
 *     Atomically read a double variable.
 */
static inline double
__wt_atomic_load_double_relaxed(double *vp)
{
    return (*vp);
}

/*
 * __wt_atomic_store_double_relaxed --
 *     Atomically set a double variable.
 */
static inline void
__wt_atomic_store_double_relaxed(double *vp, double v)
{
    *vp = v;
}

#define __wt_atomic_load_enum_relaxed(vp) (*(vp))
#define __wt_atomic_store_enum_relaxed(vp, v) (*(vp) = (v))

#define __wt_atomic_load_ptr_relaxed(vp) (*(vp))
#define __wt_atomic_store_ptr_relaxed(vp, v) (*(vp) = (v))

/*
 * Pointer atomic operations with acquire/release semantics using MSVC barrier macros. Note: These
 * are simplified macros that assume WT_ACQUIRE_READ_WITH_BARRIER and WT_RELEASE_WRITE_WITH_BARRIER
 * handle the load/store with appropriate barriers.
 */
#define __wt_atomic_load_ptr_acquire(vp) (WT_ACQUIRE_BARRIER(), *(vp))

#define __wt_atomic_store_ptr_release(vp, v) \
    do {                                     \
        WT_RELEASE_BARRIER();                \
        *(vp) = (v);                         \
    } while (0)

/*
 * __wt_atomic_cas_ptr --
 *     Pointer compare and swap.
 */
static inline bool
__wt_atomic_cas_ptr(void *vp, void *old, void *newv)
{
    return (_InterlockedCompareExchange64((volatile __int64 *)vp, (int64_t)newv, (int64_t)old) ==
      ((int64_t)old));
}

#define __wt_atomic_load_generic_relaxed(vp) (*(vp))
#define __wt_atomic_store_generic_relaxed(vp, v) (*(vp) = (v))
