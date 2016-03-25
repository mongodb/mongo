/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_PTRDIFFT_FMT	"td"			/* ptrdiff_t format string */
#define	WT_SIZET_FMT	"zu"			/* size_t format string */

#define	WT_COMPILER_TYPE_ALIGN(x)

#define	WT_PACKED_STRUCT_BEGIN(name)					\
	struct name {
#define	WT_PACKED_STRUCT_END						\
	};

#define	WT_GCC_FUNC_ATTRIBUTE(x)
#define	WT_GCC_FUNC_DECL_ATTRIBUTE(x)

#define	WT_ATOMIC_FUNC(name, ret, type)					\
static inline ret							\
__wt_atomic_add##name(type *vp, type v)					\
{									\
	*vp += v;							\
	return (*vp);							\
}									\
static inline ret							\
__wt_atomic_fetch_add##name(type *vp, type v)				\
{									\
	type orig;							\
									\
	old = *vp;							\
	*vp += v;							\
	return (old);							\
}									\
static inline ret							\
__wt_atomic_store##name(type *vp, type v)				\
{									\
	type orig;							\
									\
	orig = *vp;							\
	*vp = v;							\
	return (old);							\
}									\
static inline ret							\
__wt_atomic_sub##name(type *vp, type v)					\
{									\
	*vp -= v;							\
	return (*vp);							\
}									\
static inline bool							\
__wt_atomic_cas##name(type *vp, type old, type new)			\
{									\
	if (*vp == old) {						\
		*vp = new;						\
		return (true);						\
	}								\
	return (false);							\
}

WT_ATOMIC_FUNC(8, uint8_t, uint8_t)
WT_ATOMIC_FUNC(16, uint16_t, uint16_t)
WT_ATOMIC_FUNC(32, uint32_t, uint32_t)
WT_ATOMIC_FUNC(v32, uint32_t, volatile uint32_t)
WT_ATOMIC_FUNC(i32, int32_t, int32_t)
WT_ATOMIC_FUNC(iv32, int32_t, volatile int32_t)
WT_ATOMIC_FUNC(64, uint64_t, uint64_t)
WT_ATOMIC_FUNC(v64, uint64_t, volatile uint64_t)
WT_ATOMIC_FUNC(i64, int64_t, int64_t)
WT_ATOMIC_FUNC(iv64, int64_t, volatile int64_t)
WT_ATOMIC_FUNC(size, size_t, size_t)

/*
 * __wt_atomic_cas_ptr --
 *	Pointer compare and swap.
 */
static inline bool
__wt_atomic_cas_ptr(void *vp, void *old, void *new) {
	if (*(void **)vp == old) {
		*(void **)vp = new;
		return (true);
	}
	return (false);
}

static inline void WT_BARRIER(void) { return; }
static inline void WT_FULL_BARRIER(void) { return; }
static inline void WT_PAUSE(void) { return; }
static inline void WT_READ_BARRIER(void) { return; }
static inline void WT_WRITE_BARRIER(void) { return; }
