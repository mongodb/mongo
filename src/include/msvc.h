/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#include <intrin.h>

#ifndef _M_AMD64
#error "Only x64 is supported with MSVC"
#endif

#define	inline __inline

#define	WT_SIZET_FMT	"Iu"			/* size_t format string */

/*
 * Add MSVC-specific attributes and pragmas to types and function declarations.
 */
#define	WT_COMPILER_TYPE_ALIGN(x)	__declspec(align(x))

#define	WT_PACKED_STRUCT_BEGIN(name)					\
	__pragma(pack(push,1))						\
	struct name {

#define	WT_PACKED_STRUCT_END						\
	};								\
	__pragma(pack(pop))

#define	WT_GCC_FUNC_ATTRIBUTE(x)
#define	WT_GCC_FUNC_DECL_ATTRIBUTE(x)

#define	__WT_ATOMIC_ADD(v, val, n, s, t)				\
	(WT_STATIC_ASSERT(sizeof(v) == (n)),				\
	_InterlockedExchangeAdd ## s((t*)&(v), (t)(val)) + (val))
#define	__WT_ATOMIC_FETCH_ADD(v, val, n, s, t)				\
	(WT_STATIC_ASSERT(sizeof(v) == (n)),				\
	_InterlockedExchangeAdd ## s((t*)&(v), (t)(val)))
#define	__WT_ATOMIC_CAS(v, old, new, n, s, t)				\
	(WT_STATIC_ASSERT(sizeof(v) == (n)),				\
	_InterlockedCompareExchange ## s				\
	((t*)&(v), (t)(new), (t)(old)) == (t)(old))
#define	__WT_ATOMIC_CAS_VAL(v, old, new, n, s, t)			\
	(WT_STATIC_ASSERT(sizeof(v) == (n)),				\
	_InterlockedCompareExchange ## s((t*)&(v), (t)(new), (t)(old)))
#define	__WT_ATOMIC_STORE(v, val, n, s, t)				\
	(WT_STATIC_ASSERT(sizeof(v) == (n)),				\
	_InterlockedExchange ## s((t*)&(v), (t)(val)))
#define	__WT_ATOMIC_SUB(v, val, n, s, t)				\
	(WT_STATIC_ASSERT(sizeof(v) == (n)),				\
	_InterlockedExchangeAdd ## s((t*)&(v), -(t) val) - (val))

#define	WT_ATOMIC_ADD1(v, val)		__WT_ATOMIC_ADD(v, val, 1, 8, char)
#define	WT_ATOMIC_FETCH_ADD1(v, val)					\
	__WT_ATOMIC_FETCH_ADD(v, val, 1, 8, char)
#define	WT_ATOMIC_CAS1(v, old, new)	__WT_ATOMIC_CAS(v, old, new, 1, 8, char)
#define	WT_ATOMIC_CAS_VAL1(v, old, new)					\
	__WT_ATOMIC_CAS_VAL(v, old, new, 1, 8, char)
#define	WT_ATOMIC_STORE1(v, val)	__WT_ATOMIC_STORE(v, val, 1, 8, char)
#define	WT_ATOMIC_SUB1(v, val)		__WT_ATOMIC_SUB(v, val, 1, 8, char)

#define	WT_ATOMIC_ADD2(v, val)		__WT_ATOMIC_ADD(v, val, 2, 16, short)
#define	WT_ATOMIC_FETCH_ADD2(v, val)					\
	__WT_ATOMIC_FETCH_ADD(v, val, 2, 16, short)
#define	WT_ATOMIC_CAS2(v, old, new)					\
	__WT_ATOMIC_CAS(v, old, new, 2, 16, short)
#define	WT_ATOMIC_CAS_VAL2(v, old, new)					\
	__WT_ATOMIC_CAS_VAL(v, old, new, 2, 16, short)
#define	WT_ATOMIC_STORE2(v, val)	__WT_ATOMIC_STORE(v, val, 2, 16, short)
#define	WT_ATOMIC_SUB2(v, val)		__WT_ATOMIC_SUB(v, val, 2, 16, short)

#define	WT_ATOMIC_ADD4(v, val)		__WT_ATOMIC_ADD(v, val, 4, , long)
#define	WT_ATOMIC_FETCH_ADD4(v, val)	__WT_ATOMIC_FETCH_ADD(v, val, 4, , long)
#define	WT_ATOMIC_CAS4(v, old, new)	__WT_ATOMIC_CAS(v, old, new, 4, , long)
#define	WT_ATOMIC_CAS_VAL4(v, old, new)					\
	__WT_ATOMIC_CAS_VAL(v, old, new, 4, , long)
#define	WT_ATOMIC_STORE4(v, val)	__WT_ATOMIC_STORE(v, val, 4, , long)
#define	WT_ATOMIC_SUB4(v, val)		__WT_ATOMIC_SUB(v, val, 4, , long)

#define	WT_ATOMIC_ADD8(v, val)		__WT_ATOMIC_ADD(v, val, 8, 64, __int64)
#define	WT_ATOMIC_FETCH_ADD8(v, val)					\
	__WT_ATOMIC_FETCH_ADD(v, val, 8, 64, __int64)
#define	WT_ATOMIC_CAS8(v, old, new)					\
	__WT_ATOMIC_CAS(v, old, new, 8, 64, __int64)
#define	WT_ATOMIC_CAS_VAL8(v, old, new)					\
	__WT_ATOMIC_CAS_VAL(v, old, new, 8, 64, __int64)
#define	WT_ATOMIC_STORE8(v, val)					\
	__WT_ATOMIC_STORE(v, val, 8, 64, __int64)
#define	WT_ATOMIC_SUB8(v, val)		__WT_ATOMIC_SUB(v, val, 8, 64, __int64)

static inline void WT_BARRIER(void) { _ReadWriteBarrier(); }
static inline void WT_FULL_BARRIER(void) { _mm_mfence(); }
static inline void WT_PAUSE(void) { _mm_pause(); }
static inline void WT_READ_BARRIER(void) { _mm_lfence(); }
static inline void WT_WRITE_BARRIER(void) { _mm_sfence(); }
