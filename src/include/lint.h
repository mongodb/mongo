/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_SIZET_FMT	"zu"			/* size_t format string */

#define	WT_COMPILER_TYPE_ALIGN(x)

#define	WT_PACKED_STRUCT_BEGIN(name)					\
	struct name {
#define	WT_PACKED_STRUCT_END						\
	};

#define	WT_GCC_FUNC_ATTRIBUTE(x)
#define	WT_GCC_FUNC_DECL_ATTRIBUTE(x)

static inline uint8_t
 __wt_atomic_add1(uint8_t *vp, uint8_t v) {
	*vp += v;
	return (*vp);
}
static inline uint8_t
 __wt_atomic_fetch_add1(uint8_t *vp, uint8_t v) {
	uint8_t orig;

	old = *vp;
	*vp += v;
	return (old);
}
static inline uint8_t
 __wt_atomic_store1(uint8_t *vp, uint8_t v) {
	uint8_t orig;

	orig = *vp;
	*vp = v;
	return (old);
}
static inline uint8_t
 __wt_atomic_sub1(uint8_t *vp, uint8_t v) {
	*vp -= v;
	return (*vp);
}
static inline int
 __wt_atomic_cas1(uint8_t *vp, uint8_t old, uint8_t new) {
	if (*vp == old) {
		*vp = new;
		return (1);
	}
	return (0);
}

static inline uint16_t
 __wt_atomic_add2(uint16_t *vp, uint16_t v) {
	*vp += v;
	return (*vp);
}
static inline uint16_t
 __wt_atomic_fetch_add2(uint16_t *vp, uint16_t v) {
	uint16_t orig;

	old = *vp;
	*vp += v;
	return (old);
}
static inline uint16_t
 __wt_atomic_store2(uint16_t *vp, uint16_t v) {
	uint16_t orig;

	orig = *vp;
	*vp = v;
	return (old);
}
static inline uint16_t
 __wt_atomic_sub2(uint16_t *vp, uint16_t v) {
	*vp -= v;
	return (*vp);
}
static inline int
 __wt_atomic_cas2(uint16_t *vp, uint16_t old, uint16_t new) {
	if (*vp == old) {
		*vp = new;
		return (1);
	}
	return (0);
}

static inline int32_t
 __wt_atomic_addi4(int32_t *vp, int32_t v) {
	*vp += v;
	return (*vp);
}
static inline int32_t
 __wt_atomic_fetch_addi4(int32_t *vp, int32_t v) {
	int32_t orig;

	old = *vp;
	*vp += v;
	return (old);
}
static inline int32_t
 __wt_atomic_storei4(int32_t *vp, int32_t v) {
	int32_t orig;

	orig = *vp;
	*vp = v;
	return (old);
}
static inline int32_t
 __wt_atomic_subi4(int32_t *vp, int32_t v) {
	*vp -= v;
	return (*vp);
}
static inline int
 __wt_atomic_casi4(int32_t *vp, int32_t old, int32_t new) {
	if (*vp == old) {
		*vp = new;
		return (1);
	}
	return (0);
}

static inline uint32_t
 __wt_atomic_add4(uint32_t *vp, uint32_t v) {
	*vp += v;
	return (*vp);
}
static inline uint32_t
 __wt_atomic_fetch_add4(uint32_t *vp, uint32_t v) {
	uint32_t orig;

	old = *vp;
	*vp += v;
	return (old);
}
static inline uint32_t
 __wt_atomic_store4(uint32_t *vp, uint32_t v) {
	uint32_t orig;

	orig = *vp;
	*vp = v;
	return (old);
}
static inline uint32_t
 __wt_atomic_sub4(uint32_t *vp, uint32_t v) {
	*vp -= v;
	return (*vp);
}
static inline int
 __wt_atomic_cas4(uint32_t *vp, uint32_t old, uint32_t new) {
	if (*vp == old) {
		*vp = new;
		return (1);
	}
	return (0);
}

static inline int64_t
 __wt_atomic_addi8(int64_t *vp, int64_t v) {
	*vp += v;
	return (*vp);
}
static inline int64_t
 __wt_atomic_fetch_addi8(int64_t *vp, int64_t v) {
	int64_t orig;

	old = *vp;
	*vp += v;
	return (old);
}
static inline int64_t
 __wt_atomic_storei8(int64_t *vp, int64_t v) {
	int64_t orig;

	orig = *vp;
	*vp = v;
	return (old);
}
static inline int64_t
 __wt_atomic_subi8(int64_t *vp, int64_t v) {
	*vp -= v;
	return (*vp);
}
static inline int
 __wt_atomic_casi8(int64_t *vp, int64_t old, int64_t new) {
	if (*vp == old) {
		*vp = new;
		return (1);
	}
	return (0);
}

static inline uint64_t
 __wt_atomic_add8(uint64_t *vp, uint64_t v) {
	*vp += v;
	return (*vp);
}
static inline uint64_t
 __wt_atomic_fetch_add8(uint64_t *vp, uint64_t v) {
	uint64_t orig;

	old = *vp;
	*vp += v;
	return (old);
}
static inline uint64_t
 __wt_atomic_store8(uint64_t *vp, uint64_t v) {
	uint64_t orig;

	orig = *vp;
	*vp = v;
	return (old);
}
static inline uint64_t
 __wt_atomic_sub8(uint64_t *vp, uint64_t v) {
	*vp -= v;
	return (*vp);
}
static inline int
 __wt_atomic_cas8(uint64_t *vp, uint64_t old, uint64_t new) {
	if (*vp == old) {
		*vp = new;
		return (1);
	}
	return (0);
}
static inline int
 __wt_atomic_cas_ptr(void *vp, void *old, void *new) {
	if (*(void **)vp == old) {
		*(void **)vp = new;
		return (1);
	}
	return (0);
}

static inline void WT_BARRIER(void) { return; }
static inline void WT_FULL_BARRIER(void) { return; }
static inline void WT_PAUSE(void) { return; }
static inline void WT_READ_BARRIER(void) { return; }
static inline void WT_WRITE_BARRIER(void) { return; }
