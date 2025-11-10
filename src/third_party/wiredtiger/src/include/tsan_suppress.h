/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * This file was created for functions intended for fine-grained suppression of TSAN warnings. Since
 * TSAN only supports suppression at the function level, but a single function may trigger multiple
 * unrelated warnings, it is preferable to use one of the following functions (or create a new one)
 * to suppress them.
 *
 * We use relaxed atomic operations in these functions to provide minimal protection against
 * compiler optimizations while avoiding potential performance drops. Using atomic variables should
 * eliminate most warnings; however, we still add these functions to the suppression file to ensure
 * we don't forget to properly fix them later and to highlight that this is only a temporary
 * solution while we investigate high-priority TSAN warnings.
 */

/*
 * __wt_tsan_suppress_store_uint8_v --
 *     TSAN warnings suppression for volatile uint8 store.
 */
static WT_INLINE void
__wt_tsan_suppress_store_uint8_v(volatile uint8_t *vp, uint8_t v)
{
    __wt_atomic_store_uint8_v_relaxed(vp, v);
}

/*
 * __wt_tsan_suppress_store_uint8 --
 *     TSAN warnings suppression for uint8 store.
 */
static WT_INLINE void
__wt_tsan_suppress_store_uint8(uint8_t *vp, uint8_t v)
{
    __wt_atomic_store_uint8_relaxed(vp, v);
}

/*
 * __wt_tsan_suppress_load_uint32 --
 *     TSAN warnings suppression for uint32 load.
 */
static WT_INLINE uint32_t
__wt_tsan_suppress_load_uint32(uint32_t *vp)
{
    return (__wt_atomic_load_uint32_relaxed(vp));
}

/*
 * __wt_tsan_suppress_store_uint32 --
 *     TSAN warnings suppression for uint32 store.
 */
static WT_INLINE void
__wt_tsan_suppress_store_uint32(uint32_t *vp, uint32_t v)
{
    __wt_atomic_store_uint32_relaxed(vp, v);
}

/*
 * __wt_tsan_suppress_load_uint32_v --
 *     TSAN warnings suppression for volatile uint32 load.
 */
static WT_INLINE uint32_t
__wt_tsan_suppress_load_uint32_v(volatile uint32_t *vp)
{
    return (__wt_atomic_load_uint32_v_relaxed(vp));
}

/*
 * __wt_tsan_suppress_load_uint64 --
 *     TSAN warnings suppression for uint64 load.
 */
static WT_INLINE uint64_t
__wt_tsan_suppress_load_uint64(uint64_t *vp)
{
    return (__wt_atomic_load_uint64_relaxed(vp));
}

/*
 * __wt_tsan_suppress_load_uint64_v --
 *     TSAN warnings suppression for volatile uint64 load.
 */
static WT_INLINE uint64_t
__wt_tsan_suppress_load_uint64_v(volatile uint64_t *vp)
{
    return (__wt_atomic_load_uint64_v_relaxed(vp));
}

/*
 * __wt_tsan_suppress_store_uint64 --
 *     TSAN warnings suppression for uint64 store.
 */
static WT_INLINE void
__wt_tsan_suppress_store_uint64(uint64_t *vp, uint64_t v)
{
    __wt_atomic_store_uint64_relaxed(vp, v);
}

/*
 * __wt_tsan_suppress_add_uint64 --
 *     TSAN warnings suppression for uint64 add.
 */
static WT_INLINE void
__wt_tsan_suppress_add_uint64(uint64_t *var, uint64_t value)
{
    *var += value;
}

/*
 * __wt_tsan_suppress_sub_uint64 --
 *     TSAN warnings suppression for uint64 subtract.
 */
static WT_INLINE void
__wt_tsan_suppress_sub_uint64(uint64_t *var, uint64_t value)
{
    *var -= value;
}

/*
 * __wt_tsan_suppress_add_uint64_v --
 *     TSAN warnings suppression for volatile uint64 add.
 */
static WT_INLINE void
__wt_tsan_suppress_add_uint64_v(volatile uint64_t *var, uint64_t value)
{
    *var += value;
}

/*
 * __wt_tsan_suppress_load_size --
 *     TSAN warnings suppression for size_t load.
 */
static WT_INLINE size_t
__wt_tsan_suppress_load_size(size_t *vp)
{
    return (__wt_atomic_load_size_relaxed(vp));
}

/*
 * __wt_tsan_suppress_store_int64 --
 *     TSAN warnings suppression for int64 store.
 */
static WT_INLINE void
__wt_tsan_suppress_store_int64(int64_t *vp, int64_t v)
{
    __wt_atomic_store_int64_relaxed(vp, v);
}

/*
 * __wt_tsan_suppress_add_int64 --
 *     TSAN warnings suppression for int64 add.
 */
static WT_INLINE void
__wt_tsan_suppress_add_int64(int64_t *var, int64_t value)
{
    *var += value;
}

/*
 * __wt_tsan_suppress_sub_int64 --
 *     TSAN warnings suppression for int64 subtract.
 */
static WT_INLINE void
__wt_tsan_suppress_sub_int64(int64_t *var, int64_t value)
{
    *var -= value;
}

/*
 * __wt_tsan_suppress_load_bool --
 *     TSAN warnings suppression for bool load.
 */
static WT_INLINE bool
__wt_tsan_suppress_load_bool(bool *vp)
{
    return (__wt_atomic_load_bool_relaxed(vp));
}

/*
 * __wt_tsan_suppress_store_bool --
 *     TSAN warnings suppression for bool store.
 */
static WT_INLINE void
__wt_tsan_suppress_store_bool(bool *vp, bool v)
{
    __wt_atomic_store_bool_relaxed(vp, v);
}

/*
 * __wt_tsan_suppress_load_bool_v --
 *     TSAN warnings suppression for volatile bool load.
 */
static WT_INLINE bool
__wt_tsan_suppress_load_bool_v(volatile bool *vp)
{
    return (__wt_atomic_load_bool_v_relaxed(vp));
}

/*
 * __wt_tsan_suppress_store_bool_v --
 *     TSAN warnings suppression for volatile bool store.
 */
static WT_INLINE void
__wt_tsan_suppress_store_bool_v(volatile bool *vp, bool v)
{
    __wt_atomic_store_bool_v_relaxed(vp, v);
}

/*
 * __wt_tsan_suppress_load_pointer --
 *     TSAN warnings suppression for pointer load.
 */
static WT_INLINE void *
__wt_tsan_suppress_load_pointer(void **vp)
{
    return (__wt_atomic_load_ptr_relaxed(vp));
}

/*
 * __wt_tsan_suppress_store_pointer --
 *     TSAN warnings suppression for pointer store.
 */
static WT_INLINE void
__wt_tsan_suppress_store_pointer(void **vp, void *v)
{
    __wt_atomic_store_ptr_relaxed(vp, v);
}

/*
 * __wt_tsan_suppress_memcpy --
 *     TSAN warnings suppression for memcpy.
 */
static WT_INLINE void *
__wt_tsan_suppress_memcpy(void *dest, void *src, size_t count)
{
    return (memcpy(dest, src, count));
}

/*
 * __wt_tsan_suppress_memset --
 *     TSAN warnings suppression for memset.
 */
static WT_INLINE void *
__wt_tsan_suppress_memset(void *ptr, int val, size_t size)
{
    return (memset(ptr, val, size));
}

/*
 * __wt_tsan_suppress_load_wt_addr_ptr --
 *     TSAN warnings suppression for WT_ADDR pointer load.
 */
static WT_INLINE WT_ADDR *
__wt_tsan_suppress_load_wt_addr_ptr(void **vp)
{
    return (WT_ADDR *)(__wt_atomic_load_ptr_relaxed(vp));
}

/*
 * __wt_tsan_suppress_store_wt_addr_ptr --
 *     TSAN warnings suppression for WT_ADDR pointer store.
 */
static WT_INLINE void
__wt_tsan_suppress_store_wt_addr_ptr(void **vp, WT_ADDR *v)
{
    __wt_atomic_store_ptr_relaxed(vp, v);
}

/*
 * __wt_tsan_suppress_load_wti_logslot_ptr --
 *     TSAN warnings suppression for WTI_LOGSLOT pointer load.
 */
static WT_INLINE WTI_LOGSLOT *
__wt_tsan_suppress_load_wti_logslot_ptr(WTI_LOGSLOT **vp)
{
    return (__wt_atomic_load_ptr_relaxed(vp));
}

/*
 * __wt_tsan_suppress_load_wt_page_modify_ptr --
 *     TSAN warnings suppression for WT_PAGE_MODIFY pointer load.
 */
static WT_INLINE WT_PAGE_MODIFY *
__wt_tsan_suppress_load_wt_page_modify_ptr(WT_PAGE_MODIFY **vp)
{
    return (__wt_atomic_load_ptr_relaxed(vp));
}

/*
 * __wt_tsan_suppress_load_wt_page_header_ptr --
 *     TSAN warnings suppression for WT_PAGE_HEADER pointer load.
 */
static WT_INLINE const WT_PAGE_HEADER *
__wt_tsan_suppress_load_wt_page_header_ptr(const WT_PAGE_HEADER **vp)
{
    return (__wt_atomic_load_ptr_relaxed(vp));
}

/*
 * __wt_tsan_suppress_store_wt_page_header_ptr --
 *     TSAN warnings suppression for WT_PAGE_HEADER pointer store.
 */
static WT_INLINE void
__wt_tsan_suppress_store_wt_page_header_ptr(const WT_PAGE_HEADER **vp, const WT_PAGE_HEADER *v)
{
    __wt_atomic_store_ptr_relaxed(vp, v);
}

/*
 * __wt_tsan_suppress_load_wt_update_ptr --
 *     TSAN warnings suppression for WT_UPDATE pointer load.
 */
static WT_INLINE WT_UPDATE *
__wt_tsan_suppress_load_wt_update_ptr(WT_UPDATE **vp)
{
    return (__wt_atomic_load_ptr_relaxed(vp));
}

/*
 * __wt_tsan_suppress_load_const_char_ptr --
 *     TSAN warnings suppression for const char pointer load.
 */
static WT_INLINE const char *
__wt_tsan_suppress_load_const_char_ptr(const char **vp)
{
    return (__wt_atomic_load_ptr_relaxed(vp));
}
