/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * On systems with poor default allocators for allocations greater than 16 KB, we provide an option
 * to use TCMalloc explicitly. This is important on Windows which does not have a builtin mechanism
 * to replace C run-time memory management functions with alternatives.
 *
 * Please note that this feature has community-based support only.
 */
#ifdef ENABLE_WINDOWS_TCMALLOC_COMMUNITY_SUPPORT

#ifndef _WIN32
#error "Manual TCMalloc integration supported only on Windows."
#endif

/*
 * Include the TCMalloc header with the "-Wundef" diagnostic flag disabled. Compiling with strict
 * (where the 'Wundef' diagnostic flag is enabled), generates compilation errors where the
 * '__cplusplus' CPP macro is not defined. This being employed by the TCMalloc header to
 * differentiate C & C++ compilation environments. We don't want to define '__cplusplus' when
 * compiling C sources.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include <gperftools/tcmalloc.h>
#pragma GCC diagnostic pop

#define calloc tc_calloc
#define malloc tc_malloc
#define realloc tc_realloc
#define posix_memalign tc_posix_memalign
#define free tc_free

#endif

/*
 * __wt_calloc --
 *     ANSI calloc function.
 */
int
__wt_calloc(WT_SESSION_IMPL *session, size_t number, size_t size, void *retp)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    void *p;

    /*
     * Defensive: if our caller doesn't handle errors correctly, ensure a free won't fail.
     */
    *(void **)retp = NULL;

    /*
     * !!!
     * This function MUST handle a NULL WT_SESSION_IMPL handle.
     */
    WT_ASSERT(session, number != 0 && size != 0);

    if (session != NULL)
        WT_STAT_CONN_INCR(session, memory_allocation);

    if ((p = calloc(number, size)) == NULL)
        WT_RET_MSG(session, __wt_errno(), "memory allocation of %" WT_SIZET_FMT " bytes failed",
          size * number);

    *(void **)retp = p;
    return (0);
}

/*
 * __wt_malloc --
 *     ANSI malloc function.
 */
int
__wt_malloc(WT_SESSION_IMPL *session, size_t bytes_to_allocate, void *retp)
{
    void *p;

    /*
     * Defensive: if our caller doesn't handle errors correctly, ensure a free won't fail.
     */
    *(void **)retp = NULL;

    /*
     * !!!
     * This function MUST handle a NULL WT_SESSION_IMPL handle.
     */
    WT_ASSERT(session, bytes_to_allocate != 0);

    if (session != NULL)
        WT_STAT_CONN_INCR(session, memory_allocation);

    if ((p = malloc(bytes_to_allocate)) == NULL)
        WT_RET_MSG(session, __wt_errno(), "memory allocation of %" WT_SIZET_FMT " bytes failed",
          bytes_to_allocate);

    *(void **)retp = p;
    return (0);
}

/*
 * __realloc_func --
 *     ANSI realloc function.
 */
static int
__realloc_func(WT_SESSION_IMPL *session, size_t *bytes_allocated_ret, size_t bytes_to_allocate,
  bool clear_memory, void *retp)
{
    size_t bytes_allocated;
    void *p, *tmpp;

    WT_ASSERT_ALWAYS(session, !(bytes_allocated_ret == NULL && clear_memory),
      "bytes allocated must be passed in if clear_memory is set, otherwise use "
      "__wt_realloc_noclear");

    /*
     * !!!
     * This function MUST handle a NULL WT_SESSION_IMPL handle.
     *
     * Sometimes we're allocating memory and we don't care about the
     * final length -- bytes_allocated_ret may be NULL.
     */
    p = *(void **)retp;
    bytes_allocated = (bytes_allocated_ret == NULL) ? 0 : *bytes_allocated_ret;
    WT_ASSERT(session,
      (p == NULL && bytes_allocated == 0) ||
        (p != NULL && (bytes_allocated_ret == NULL || bytes_allocated != 0)));
    WT_ASSERT(session, bytes_to_allocate != 0);
    WT_ASSERT(session, bytes_allocated < bytes_to_allocate);

    if (session != NULL) {
        if (p == NULL)
            WT_STAT_CONN_INCR(session, memory_allocation);
        else
            WT_STAT_CONN_INCR(session, memory_grow);
    }

    /*
     * If realloc_malloc is enabled, force a new memory allocation by using malloc, copy to the new
     * memory, scribble over the old memory then free it.
     */
    tmpp = p;
    if (session != NULL && FLD_ISSET(S2C(session)->debug_flags, WT_CONN_DEBUG_REALLOC_MALLOC) &&
      (bytes_allocated_ret != NULL)) {
        if ((p = malloc(bytes_to_allocate)) == NULL)
            WT_RET_MSG(session, __wt_errno(), "memory allocation of %" WT_SIZET_FMT " bytes failed",
              bytes_to_allocate);
        if (tmpp != NULL) {
            memcpy(p, tmpp, *bytes_allocated_ret);
            __wt_explicit_overwrite(tmpp, bytes_allocated);
            __wt_free(session, tmpp);
        }
    } else {
        if ((p = realloc(p, bytes_to_allocate)) == NULL)
            WT_RET_MSG(session, __wt_errno(), "memory allocation of %" WT_SIZET_FMT " bytes failed",
              bytes_to_allocate);
    }

    /*
     * Clear the allocated memory, parts of WiredTiger depend on allocated memory being cleared.
     */
    if (clear_memory)
        memset((uint8_t *)p + bytes_allocated, 0, bytes_to_allocate - bytes_allocated);

    /* Update caller's bytes allocated value. */
    if (bytes_allocated_ret != NULL)
        *bytes_allocated_ret = bytes_to_allocate;

    *(void **)retp = p;
    return (0);
}

/*
 * __wt_realloc --
 *     WiredTiger's realloc API.
 */
int
__wt_realloc(
  WT_SESSION_IMPL *session, size_t *bytes_allocated_ret, size_t bytes_to_allocate, void *retp)
{
    return (__realloc_func(session, bytes_allocated_ret, bytes_to_allocate, true, retp));
}

/*
 * __wt_realloc_noclear --
 *     WiredTiger's realloc API, not clearing allocated memory.
 */
int
__wt_realloc_noclear(
  WT_SESSION_IMPL *session, size_t *bytes_allocated_ret, size_t bytes_to_allocate, void *retp)
{
    return (__realloc_func(session, bytes_allocated_ret, bytes_to_allocate, false, retp));
}

/*
 * __wt_memdup --
 *     Duplicate a byte string of a given length.
 */
int
__wt_memdup(WT_SESSION_IMPL *session, const void *str, size_t len, void *retp)
{
    void *p;

    WT_RET(__wt_malloc(session, len, &p));

    WT_ASSERT(session, p != NULL); /* quiet clang scan-build */

    memcpy(p, str, len);

    *(void **)retp = p;
    return (0);
}

/*
 * __wt_strndup --
 *     ANSI strndup function.
 */
int
__wt_strndup(WT_SESSION_IMPL *session, const void *str, size_t len, void *retp)
{
    uint8_t *p;

    if (str == NULL) {
        *(void **)retp = NULL;
        return (0);
    }

    /* Copy and nul-terminate. */
    WT_RET(__wt_malloc(session, len + 1, &p));

    WT_ASSERT(session, p != NULL); /* quiet clang scan-build */

    memcpy(p, str, len);
    p[len] = '\0';

    *(void **)retp = p;
    return (0);
}

/*
 * __wt_free_int --
 *     ANSI free function.
 */
void
__wt_free_int(WT_SESSION_IMPL *session, const void *p_arg)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    void *p;

    p = *(void **)p_arg;
    if (p == NULL) /* ANSI C free semantics */
        return;

    /*
     * If there's a serialization bug we might race with another thread. We can't avoid the race
     * (and we aren't willing to flush memory), but we minimize the window by clearing the free
     * address, hoping a racing thread will see, and won't free, a NULL pointer.
     */
    *(void **)p_arg = NULL;

    /*
     * !!!
     * This function MUST handle a NULL WT_SESSION_IMPL handle.
     */
    if (session != NULL)
        WT_STAT_CONN_INCR(session, memory_free);

    free(p);
}
