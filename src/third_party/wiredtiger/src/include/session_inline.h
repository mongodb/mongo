/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * __wt_single_thread_check_start --
 *     Only a single thread should use this session at a time. It's ok (but unexpected) if different
 *     threads use the session consecutively, but concurrent access is not allowed. Verify this by
 *     having the thread take a lock on first API access. Failing to take the lock implies another
 *     thread holds it and we're attempting concurrent access of the session.
 *
 * The default session (ID == 0) is an exception where concurrent access is allowed. We can also
 *     skip taking the lock if we're re-entrant and already hold it.
 */
static WT_INLINE void
__wt_single_thread_check_start(WT_SESSION_IMPL *s)
{
#if !defined(HAVE_DIAGNOSTIC)
    WT_UNUSED(s);
    return;
#else
    uintmax_t current_tid;
    WT_DECL_RET;

    __wt_thread_id(&current_tid);
    if (!WT_SESSION_IS_DEFAULT(s) && s->thread_check.owning_thread != current_tid) {
        ret = __wt_spin_trylock(s, &s->thread_check.lock);

        WT_ASSERT_ALWAYS(s, ret == 0,
          "Session %" PRIu32
          " is accessed concurrently by multiple threads: "
          "current thread %" PRIuMAX ", owning thread %" PRIuMAX
          " (active op: %s, last op: %s, api depth: %u, dhandle: %s)",
          s->id, current_tid, s->thread_check.owning_thread, s->name != NULL ? s->name : "none",
          s->lastop != NULL ? s->lastop : "none", s->api_call_counter,
          s->dhandle != NULL ? s->dhandle->name : "none");

        s->thread_check.owning_thread = current_tid;
    }
    ++s->thread_check.entry_count;
#endif
}

/*
 * __wt_single_thread_check_stop --
 *     Release the single-thread ownership of this session.
 */
static WT_INLINE void
__wt_single_thread_check_stop(WT_SESSION_IMPL *s)
{
#if !defined(HAVE_DIAGNOSTIC)
    WT_UNUSED(s);
    return;
#else
    if (--s->thread_check.entry_count == 0 && !WT_SESSION_IS_DEFAULT(s)) {
        s->thread_check.owning_thread = 0;
        __wt_spin_unlock(s, &s->thread_check.lock);
    }
#endif
}
