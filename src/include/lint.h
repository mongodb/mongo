/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_GCC_ATTRIBUTE(x)
#define	WT_GCC_FUNC_ATTRIBUTE(x)

#define	__WT_ATOMIC_ADD(v, val)						\
    ((v) += (val))
#define	__WT_ATOMIC_FETCH_ADD(v, val)					\
    ((v) += (val), (v))
#define	__WT_ATOMIC_CAS(v, old, new)					\
    ((v) = ((v) == (old) ? (new) : (old)), (v) == (old))
#define	__WT_ATOMIC_CAS_VAL(v, old, new)				\
    ((v) = ((v) == (old) ? (new) : (old)), (v) == (old))
#define	__WT_ATOMIC_STORE(v, val)					\
    ((v) = (val))
#define	__WT_ATOMIC_SUB(v, val)						\
    ((v) -= (val), (v))

#define	WT_ATOMIC_ADD1(v, val)		__WT_ATOMIC_ADD(v, val)
#define	WT_ATOMIC_FETCH_ADD1(v, val)	__WT_ATOMIC_FETCH_ADD(v, val)
#define	WT_ATOMIC_CAS1(v, old, new)	__WT_ATOMIC_CAS(v, old, new)
#define	WT_ATOMIC_CAS_VAL1(v, old, new)	__WT_ATOMIC_CAS_VAL(v, old, new)
#define	WT_ATOMIC_STORE1(v, val)	__WT_ATOMIC_STORE(v, val)
#define	WT_ATOMIC_SUB1(v, val)		__WT_ATOMIC_SUB(v, val)

#define	WT_ATOMIC_ADD2(v, val)		__WT_ATOMIC_ADD(v, val)
#define	WT_ATOMIC_FETCH_ADD2(v, val)	__WT_ATOMIC_FETCH_ADD(v, val)
#define	WT_ATOMIC_CAS2(v, old, new)	__WT_ATOMIC_CAS(v, old, new)
#define	WT_ATOMIC_CAS_VAL2(v, old, new)	__WT_ATOMIC_CAS_VAL(v, old, new)
#define	WT_ATOMIC_STORE2(v, val)	__WT_ATOMIC_STORE(v, val)
#define	WT_ATOMIC_SUB2(v, val)		__WT_ATOMIC_SUB(v, val)

#define	WT_ATOMIC_ADD4(v, val)		__WT_ATOMIC_ADD(v, val)
#define	WT_ATOMIC_FETCH_ADD4(v, val)	__WT_ATOMIC_FETCH_ADD(v, val)
#define	WT_ATOMIC_CAS4(v, old, new)	__WT_ATOMIC_CAS(v, old, new)
#define	WT_ATOMIC_CAS_VAL4(v, old, new)	__WT_ATOMIC_CAS_VAL(v, old, new)
#define	WT_ATOMIC_STORE4(v, val)	__WT_ATOMIC_STORE(v, val)
#define	WT_ATOMIC_SUB4(v, val)		__WT_ATOMIC_SUB(v, val)

#define	WT_ATOMIC_ADD8(v, val)		__WT_ATOMIC_ADD(v, val)
#define	WT_ATOMIC_FETCH_ADD8(v, val)	__WT_ATOMIC_FETCH_ADD(v, val)
#define	WT_ATOMIC_CAS8(v, old, new)	__WT_ATOMIC_CAS(v, old, new)
#define	WT_ATOMIC_CAS_VAL8(v, old, new)	__WT_ATOMIC_CAS_VAL(v, old, new)
#define	WT_ATOMIC_STORE8(v, val)	__WT_ATOMIC_STORE(v, val)
#define	WT_ATOMIC_SUB8(v, val)		__WT_ATOMIC_SUB(v, val)

static inline void WT_BARRIER(void) { return; }
static inline void WT_FULL_BARRIER(void) { return; }
static inline void WT_PAUSE(void) { return; }
static inline void WT_READ_BARRIER(void) { return; }
static inline void WT_WRITE_BARRIER(void) { return; }
