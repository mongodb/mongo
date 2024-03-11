/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * WiredTiger uses generations to manage various resources. Threads publish a current generation
 * before accessing a resource, and clear it when they are done. For example, a thread wanting to
 * replace an object in memory replaces the object and increments the object's generation. Once no
 * threads have the previous generation published, it is safe to discard the previous version of the
 * object.
 */

/*
 * __gen_name --
 *     Return the generation name.
 */
static const char *
__gen_name(int which)
{
    switch (which) {
    case WT_GEN_CHECKPOINT:
        return ("checkpoint");
    case WT_GEN_EVICT:
        return ("evict");
    case WT_GEN_HAS_SNAPSHOT:
        return ("snapshot");
    case WT_GEN_HAZARD:
        return ("hazard");
    case WT_GEN_SPLIT:
        return ("split");
    default:
        break;
    }
    return ("unknown");
}

/*
 * __wt_gen_init --
 *     Initialize the connection's generations.
 */
void
__wt_gen_init(WT_SESSION_IMPL *session)
{
    int i;

    /*
     * All generations start at 1, a session with a generation of 0 isn't using the resource.
     */
    for (i = 0; i < WT_GENERATIONS; ++i)
        __wt_atomic_storev64(&S2C(session)->generations[i], 1);

    /* Ensure threads see the state change. */
    WT_RELEASE_BARRIER();
}

/*
 * __wt_gen --
 *     Return the resource's generation.
 */
uint64_t
__wt_gen(WT_SESSION_IMPL *session, int which)
{
    return (__wt_atomic_loadv64(&S2C(session)->generations[which]));
}

/*
 * __wt_gen_next --
 *     Switch the resource to its next generation.
 */
void
__wt_gen_next(WT_SESSION_IMPL *session, int which, uint64_t *genp)
{
    uint64_t gen;

    gen = __wt_atomic_addv64(&S2C(session)->generations[which], 1);
    if (genp != NULL)
        *genp = gen;
}

/*
 * __wt_gen_next_drain --
 *     Switch the resource to its next generation, then wait for it to drain.
 */
void
__wt_gen_next_drain(WT_SESSION_IMPL *session, int which)
{
    uint64_t v;

    v = __wt_atomic_addv64(&S2C(session)->generations[which], 1);

    __wt_gen_drain(session, which, v);
}

/*
 * __gen_drain_callback --
 *     Wait for single session's generation to drain. Callback from the session array walk.
 */
static int
__gen_drain_callback(
  WT_SESSION_IMPL *session, WT_SESSION_IMPL *array_session, bool *exit_walkp, void *cookiep)
{
    struct timespec stop;
    WT_CONNECTION_IMPL *conn;
    WT_GENERATION_DRAIN_COOKIE *cookie;
    uint64_t time_diff_ms, v;
#ifdef HAVE_DIAGNOSTIC
    WT_VERBOSE_LEVEL verbose_orig_level[WT_VERB_NUM_CATEGORIES];
#endif

    cookie = (WT_GENERATION_DRAIN_COOKIE *)cookiep;
    conn = S2C(session);

    for (;;) {
        /* Ensure we only read the value once. */
        WT_ACQUIRE_READ_WITH_BARRIER(v, array_session->generations[cookie->base.which]);

        /*
         * The generation argument is newer than the limit. Wait for threads in generations older
         * than the argument generation, threads in argument generations are OK.
         *
         * The thread's generation may be 0 (that is, not set).
         */
        if (v == 0 || v >= cookie->base.target_generation) {
#ifdef HAVE_DIAGNOSTIC
            /*
             * We turn on additional logging just before generation drain times out, but it's
             * possible that we get unblocked after increasing the traces but before hitting the
             * timeout. If this occurs set verbose levels back to their original values so we can
             * continue normal operation.
             */
            if (cookie->verbose_timeout_flags == true) {
                if (cookie->base.which == WT_GEN_EVICT) {
                    WT_VERBOSE_RESTORE(session, verbose_orig_level, WT_VERB_EVICT);
                    WT_VERBOSE_RESTORE(session, verbose_orig_level, WT_VERB_EVICTSERVER);
                    WT_VERBOSE_RESTORE(session, verbose_orig_level, WT_VERB_EVICT_STUCK);
                } else if (cookie->base.which == WT_GEN_CHECKPOINT) {
                    WT_VERBOSE_RESTORE(session, verbose_orig_level, WT_VERB_CHECKPOINT);
                    WT_VERBOSE_RESTORE(session, verbose_orig_level, WT_VERB_CHECKPOINT_CLEANUP);
                    WT_VERBOSE_RESTORE(session, verbose_orig_level, WT_VERB_CHECKPOINT_PROGRESS);
                }
            }
#endif
            break;
        }
        /* If we're waiting on ourselves, we're deadlocked. */
        if (array_session == session) {
            WT_IGNORE_RET(__wt_panic(array_session, WT_PANIC, "self-deadlock"));
            *exit_walkp = true;
            return (0);
        }

        /*
         * The pause count is cumulative, quit spinning if it's not doing us any good, that can
         * happen in generations that don't move quickly.
         */
        if (++cookie->pause_cnt < WT_THOUSAND)
            WT_PAUSE();
        else
            __wt_sleep(0, 10);

        /*
         * If we wait for more than a minute, log the event. In diagnostic mode, abort if we ever
         * wait more than the configured timeout.
         */
        if (cookie->minutes == 0) {
            cookie->minutes = 1;
            __wt_epoch(session, &cookie->start);
        } else {
            __wt_epoch(session, &stop);
            time_diff_ms = WT_TIMEDIFF_MS(stop, cookie->start);

            if (time_diff_ms > cookie->minutes * WT_MINUTE * WT_THOUSAND) {
                __wt_verbose_notice(session, WT_VERB_GENERATION,
                  "%s generation drain waited %" PRIu64 " minutes", __gen_name(cookie->base.which),
                  cookie->minutes);
                ++cookie->minutes;
            }

            /* If there is no timeout, there is nothing else to do. */
            if (conn->gen_drain_timeout_ms == 0)
                continue;

#ifdef HAVE_DIAGNOSTIC
            /* In diagnostic mode, enable extra logs 20ms before reaching the timeout. */
            if (!cookie->verbose_timeout_flags &&
              (conn->gen_drain_timeout_ms < 20 ||
                time_diff_ms > (conn->gen_drain_timeout_ms - 20))) {
                if (cookie->base.which == WT_GEN_EVICT) {
                    WT_VERBOSE_SET_AND_SAVE(
                      session, verbose_orig_level, WT_VERB_EVICT, WT_VERBOSE_DEBUG_1);
                    WT_VERBOSE_SET_AND_SAVE(
                      session, verbose_orig_level, WT_VERB_EVICTSERVER, WT_VERBOSE_DEBUG_1);
                    WT_VERBOSE_SET_AND_SAVE(
                      session, verbose_orig_level, WT_VERB_EVICT_STUCK, WT_VERBOSE_DEBUG_1);
                } else if (cookie->base.which == WT_GEN_CHECKPOINT) {
                    WT_VERBOSE_SET_AND_SAVE(
                      session, verbose_orig_level, WT_VERB_CHECKPOINT, WT_VERBOSE_DEBUG_1);
                    WT_VERBOSE_SET_AND_SAVE(
                      session, verbose_orig_level, WT_VERB_CHECKPOINT_CLEANUP, WT_VERBOSE_DEBUG_1);
                    WT_VERBOSE_SET_AND_SAVE(
                      session, verbose_orig_level, WT_VERB_CHECKPOINT_PROGRESS, WT_VERBOSE_DEBUG_1);
                }
                cookie->verbose_timeout_flags = true;
                /* Now we have enabled more logs, spin another time to get some information. */
                continue;
            }
#endif
            if (time_diff_ms >= conn->gen_drain_timeout_ms) {
                __wt_verbose_error(session, WT_VERB_GENERATION, "%s generation drain timed out",
                  __gen_name(cookie->base.which));
                WT_ASSERT(session, false);
            }
        }
    }

    return (0);
}

/*
 * __wt_gen_drain --
 *     Wait for the resource to drain.
 */
void
__wt_gen_drain(WT_SESSION_IMPL *session, int which, uint64_t generation)
{
    WT_GENERATION_DRAIN_COOKIE cookie;

    WT_CLEAR(cookie);
    cookie.base.which = which;
    cookie.base.target_generation = generation;

    __wt_epoch(session, &cookie.start);
    WT_IGNORE_RET(__wt_session_array_walk(session, __gen_drain_callback, false, &cookie));
}

/*
 * __gen_oldest_callback --
 *     Check a single session's generation to find the oldest. Callback from the session array walk.
 */
static int
__gen_oldest_callback(
  WT_SESSION_IMPL *session, WT_SESSION_IMPL *array_session, bool *exit_walkp, void *cookiep)
{
    WT_GENERATION_COOKIE *cookie;
    uint64_t v;

    WT_UNUSED(session);
    WT_UNUSED(exit_walkp);
    cookie = (WT_GENERATION_COOKIE *)cookiep;

    WT_ACQUIRE_READ_WITH_BARRIER(v, array_session->generations[cookie->which]);
    if (v != 0 && v < cookie->ret_oldest_gen)
        cookie->ret_oldest_gen = v;

    return (0);
}

/*
 * __gen_oldest --
 *     Return the oldest generation in use for the resource.
 */
static uint64_t
__gen_oldest(WT_SESSION_IMPL *session, int which)
{
    WT_CONNECTION_IMPL *conn;
    WT_GENERATION_COOKIE cookie;

    conn = S2C(session);
    WT_CLEAR(cookie);
    cookie.which = which;

    /*
     * We need to order the read of the connection generation before the read of the session
     * generation. If the session generation read is ordered before the connection generation read
     * it could read an earlier session generation value. This would then violate the acquisition
     * semantics and could result in us reading 0 for the session generation when it is non-zero.
     */
    WT_ACQUIRE_READ_WITH_BARRIER(cookie.ret_oldest_gen, conn->generations[which]);

    WT_IGNORE_RET(__wt_session_array_walk(session, __gen_oldest_callback, false, &cookie));

    return (cookie.ret_oldest_gen);
}

/*
 * __gen_active_callback --
 *     Check if a session's generation is relevant given a specific generation. Callback from the
 *     session array walk.
 */
static int
__gen_active_callback(
  WT_SESSION_IMPL *session, WT_SESSION_IMPL *array_session, bool *exit_walkp, void *cookiep)
{
    WT_GENERATION_COOKIE *cookie;
    uint64_t v;

    WT_UNUSED(session);
    cookie = (WT_GENERATION_COOKIE *)cookiep;

    WT_ACQUIRE_READ_WITH_BARRIER(v, array_session->generations[cookie->which]);
    if (v != 0 && cookie->target_generation >= v) {
        cookie->ret_active = true;
        *exit_walkp = true;
    }

    return (0);
}

/*
 * __wt_gen_active --
 *     Return if a specified generation is in use for the resource.
 */
bool
__wt_gen_active(WT_SESSION_IMPL *session, int which, uint64_t generation)
{
    WT_GENERATION_COOKIE cookie;

    WT_CLEAR(cookie);
    cookie.which = which;
    cookie.target_generation = generation;
    cookie.ret_active = false;

    WT_IGNORE_RET(__wt_session_array_walk(session, __gen_active_callback, false, &cookie));

    return (cookie.ret_active);
}

/*
 * __wt_session_gen --
 *     Return the thread's resource generation.
 */
uint64_t
__wt_session_gen(WT_SESSION_IMPL *session, int which)
{
    return (__wt_atomic_loadv64(&session->generations[which]));
}

/*
 * __wt_session_gen_enter --
 *     Publish a thread's resource generation.
 */
void
__wt_session_gen_enter(WT_SESSION_IMPL *session, int which)
{
    /*
     * Don't enter a generation we're already in, it will likely result in code intended to be
     * protected by a generation running outside one.
     */
    WT_ASSERT(session, __wt_atomic_loadv64(&session->generations[which]) == 0);
    WT_ASSERT(session, session->active);
    WT_ASSERT(session, session->id < __wt_atomic_load32(&S2C(session)->session_array.cnt));

    /*
     * Assign the thread's resource generation, ensuring threads waiting on a resource to drain see
     * the new value. Check we haven't raced with a generation update after assigning, we rely on
     * the new value not being missed when scanning for the oldest generation and for draining.
     *
     * This requires a full barrier as the second read of the connection generation needs to be
     * ordered after the write of our session's generation. If it is reordered it could be read, for
     * example before we do the first read. This would make re-checking redundant and in this case
     * can result in the generation drain and generation oldest code not working correctly.
     */
    do {
        __wt_atomic_storev64(&session->generations[which], __wt_gen(session, which));
        WT_FULL_BARRIER();
    } while (__wt_atomic_loadv64(&session->generations[which]) != __wt_gen(session, which));
}

/*
 * __wt_session_gen_leave --
 *     Leave a thread's resource generation.
 */
void
__wt_session_gen_leave(WT_SESSION_IMPL *session, int which)
{
    WT_ASSERT(session, session->active);
    WT_ASSERT(session, session->id < __wt_atomic_load32(&S2C(session)->session_array.cnt));

    /* Ensure writes made by this thread are visible. */
    WT_RELEASE_WRITE_WITH_BARRIER(session->generations[which], 0);

    /* Let threads waiting for the resource to drain proceed quickly. */
    WT_FULL_BARRIER();
}

/*
 * __stash_discard --
 *     Discard any memory from a session stash that we can.
 */
static void
__stash_discard(WT_SESSION_IMPL *session, int which)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_STASH *session_stash;
    WT_STASH *stash;
    size_t i;
    uint64_t oldest;

    conn = S2C(session);
    session_stash = &session->stash[which];

    /* Get the resource's oldest generation. */
    oldest = __gen_oldest(session, which);

    for (i = 0, stash = session_stash->list; i < session_stash->cnt; ++i, ++stash) {
        if (stash->p == NULL)
            continue;
        /*
         * The list is expected to be in generation-sorted order, quit as soon as we find a object
         * we can't discard.
         */
        if (stash->gen >= oldest)
            break;

        (void)__wt_atomic_sub64(&conn->stashed_bytes, stash->len);
        (void)__wt_atomic_sub64(&conn->stashed_objects, 1);

        /*
         * It's a bad thing if another thread is in this memory after we free it, make sure nothing
         * good happens to that thread.
         */
        __wt_overwrite_and_free_len(session, stash->p, stash->len);
    }

    /*
     * If there are enough free slots at the beginning of the list, shuffle everything down.
     */
    if (i > 100 || i == session_stash->cnt)
        if ((session_stash->cnt -= i) > 0)
            memmove(session_stash->list, stash, session_stash->cnt * sizeof(*stash));
}

/*
 * __wt_stash_discard --
 *     Discard any memory from a session stash that we can.
 */
void
__wt_stash_discard(WT_SESSION_IMPL *session)
{
    WT_SESSION_STASH *session_stash;
    int which;

    for (which = 0; which < WT_GENERATIONS; ++which) {
        session_stash = &session->stash[which];
        if (session_stash->cnt >= 1)
            __stash_discard(session, which);
    }
}

/*
 * __wt_stash_add --
 *     Add a new entry into a session stash list.
 */
int
__wt_stash_add(WT_SESSION_IMPL *session, int which, uint64_t generation, void *p, size_t len)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_STASH *session_stash;
    WT_STASH *stash;

    conn = S2C(session);
    session_stash = &session->stash[which];

    /* Grow the list as necessary. */
    WT_RET(__wt_realloc_def(
      session, &session_stash->alloc, session_stash->cnt + 1, &session_stash->list));

    /*
     * If no caller stashes memory with a lower generation than a previously stashed object, the
     * list is in generation-sorted order and discarding can be faster. (An error won't cause
     * problems other than we might not discard stashed objects as soon as we otherwise would have.)
     */
    stash = session_stash->list + session_stash->cnt++;
    stash->p = p;
    stash->len = len;
    stash->gen = generation;

    (void)__wt_atomic_add64(&conn->stashed_bytes, len);
    (void)__wt_atomic_add64(&conn->stashed_objects, 1);

    /* See if we can free any previous entries. */
    if (session_stash->cnt > 1)
        __stash_discard(session, which);

    return (0);
}

/*
 * __wt_stash_discard_all --
 *     Discard all memory from a session's stash.
 */
void
__wt_stash_discard_all(WT_SESSION_IMPL *session_safe, WT_SESSION_IMPL *session)
{
    WT_SESSION_STASH *session_stash;
    WT_STASH *stash;
    size_t i;
    int which;

    /*
     * This function is called during WT_CONNECTION.close to discard any memory that remains. For
     * that reason, we take two WT_SESSION_IMPL arguments: session_safe is still linked to the
     * WT_CONNECTION and can be safely used for calls to other WiredTiger functions, while session
     * is the WT_SESSION_IMPL we're cleaning up.
     */
    for (which = 0; which < WT_GENERATIONS; ++which) {
        session_stash = &session->stash[which];

        for (i = 0, stash = session_stash->list; i < session_stash->cnt; ++i, ++stash)
            __wt_free(session_safe, stash->p);

        __wt_free(session_safe, session_stash->list);
        session_stash->cnt = session_stash->alloc = 0;
    }
}
