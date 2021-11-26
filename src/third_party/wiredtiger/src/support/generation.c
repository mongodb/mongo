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
    case WT_GEN_COMMIT:
        return ("commit");
    case WT_GEN_EVICT:
        return ("evict");
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
        S2C(session)->generations[i] = 1;

    /* Ensure threads see the state change. */
    WT_WRITE_BARRIER();
}

/*
 * __wt_gen --
 *     Return the resource's generation.
 */
uint64_t
__wt_gen(WT_SESSION_IMPL *session, int which)
{
    return (S2C(session)->generations[which]);
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
 * __wt_gen_drain --
 *     Wait for the resource to drain.
 */
void
__wt_gen_drain(WT_SESSION_IMPL *session, int which, uint64_t generation)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *s;
    uint64_t v, start, stop;
    uint32_t i, session_cnt;
    u_int minutes;
    int pause_cnt;

    conn = S2C(session);
    start = 0; /* [-Wconditional-uninitialized] */

    /*
     * No lock is required because the session array is fixed size, but it may contain inactive
     * entries. We must review any active session, so insert a read barrier after reading the active
     * session count. That way, no matter what sessions come or go, we'll check the slots for all of
     * the sessions that could have been active when we started our check.
     */
    WT_ORDERED_READ(session_cnt, conn->session_cnt);
    for (minutes = 0, pause_cnt = 0, s = conn->sessions, i = 0; i < session_cnt; ++s, ++i) {
        if (!s->active)
            continue;

        for (;;) {
            /* Ensure we only read the value once. */
            WT_ORDERED_READ(v, s->generations[which]);

            /*
             * The generation argument is newer than the limit. Wait for threads in generations
             * older than the argument generation, threads in argument generations are OK.
             *
             * The thread's generation may be 0 (that is, not set).
             */
            if (v == 0 || v >= generation)
                break;

            /* If we're waiting on ourselves, we're deadlocked. */
            if (session == s) {
                WT_IGNORE_RET(__wt_panic(session, WT_PANIC, "self-deadlock"));
                return;
            }

            /*
             * The pause count is cumulative, quit spinning if it's not doing us any good, that can
             * happen in generations that don't move quickly.
             */
            if (++pause_cnt < WT_THOUSAND)
                WT_PAUSE();
            else
                __wt_sleep(0, 10);

            /*
             * If we wait for more than a minute, log the event. In DIAGNOSTIC mode, abort if we
             * ever wait more than 3 minutes, that's forever.
             */
            if (minutes == 0) {
                minutes = 1;
                __wt_seconds(session, &start);
            } else {
                __wt_seconds(session, &stop);
                if (stop - start > minutes * WT_MINUTE) {
                    __wt_verbose_notice(session, WT_VERB_GENERATION,
                      "%s generation drain waited %u minutes", __gen_name(which), minutes);
                    ++minutes;
                    WT_ASSERT(session, minutes < 4);
                }
            }
        }
    }
}

/*
 * __gen_oldest --
 *     Return the oldest generation in use for the resource.
 */
static uint64_t
__gen_oldest(WT_SESSION_IMPL *session, int which)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *s;
    uint64_t oldest, v;
    uint32_t i, session_cnt;

    conn = S2C(session);

    /*
     * No lock is required because the session array is fixed size, but it may contain inactive
     * entries. We must review any active session, so insert a read barrier after reading the active
     * session count. That way, no matter what sessions come or go, we'll check the slots for all of
     * the sessions that could have been active when we started our check.
     */
    WT_ORDERED_READ(session_cnt, conn->session_cnt);
    for (oldest = conn->generations[which], s = conn->sessions, i = 0; i < session_cnt; ++s, ++i) {
        if (!s->active)
            continue;

        /* Ensure we only read the value once. */
        WT_ORDERED_READ(v, s->generations[which]);

        if (v != 0 && v < oldest)
            oldest = v;
    }

    return (oldest);
}

/*
 * __wt_gen_active --
 *     Return if a specified generation is in use for the resource.
 */
bool
__wt_gen_active(WT_SESSION_IMPL *session, int which, uint64_t generation)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *s;
    uint64_t v;
    uint32_t i, session_cnt;

    conn = S2C(session);

    /*
     * No lock is required because the session array is fixed size, but it may contain inactive
     * entries. We must review any active session, so insert a read barrier after reading the active
     * session count. That way, no matter what sessions come or go, we'll check the slots for all of
     * the sessions that could have been active when we started our check.
     */
    WT_ORDERED_READ(session_cnt, conn->session_cnt);
    for (s = conn->sessions, i = 0; i < session_cnt; ++s, ++i) {
        if (!s->active)
            continue;

        /* Ensure we only read the value once. */
        WT_ORDERED_READ(v, s->generations[which]);

        if (v != 0 && generation >= v)
            return (true);
    }

#ifdef HAVE_DIAGNOSTIC
    {
        uint64_t oldest = __gen_oldest(session, which);
        WT_ASSERT(session, generation < oldest);
    }
#endif
    return (false);
}

/*
 * __wt_session_gen --
 *     Return the thread's resource generation.
 */
uint64_t
__wt_session_gen(WT_SESSION_IMPL *session, int which)
{
    return (session->generations[which]);
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
    WT_ASSERT(session, session->generations[which] == 0);
    WT_ASSERT(session, session->active);
    WT_ASSERT(session, session->id < S2C(session)->session_cnt);

    /*
     * Assign the thread's resource generation and publish it, ensuring threads waiting on a
     * resource to drain see the new value. Check we haven't raced with a generation update after
     * publishing, we rely on the published value not being missed when scanning for the oldest
     * generation.
     */
    do {
        session->generations[which] = __wt_gen(session, which);
        WT_WRITE_BARRIER();
    } while (session->generations[which] != __wt_gen(session, which));
}

/*
 * __wt_session_gen_leave --
 *     Leave a thread's resource generation.
 */
void
__wt_session_gen_leave(WT_SESSION_IMPL *session, int which)
{
    WT_ASSERT(session, session->active);
    WT_ASSERT(session, session->id < S2C(session)->session_cnt);

    /* Ensure writes made by this thread are visible. */
    WT_PUBLISH(session->generations[which], 0);

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
