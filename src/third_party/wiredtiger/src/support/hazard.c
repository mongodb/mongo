/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#ifdef HAVE_DIAGNOSTIC
static void __hazard_dump(WT_SESSION_IMPL *);
#endif

/*
 * hazard_grow --
 *     Grow a hazard pointer array.
 */
static int
hazard_grow(WT_SESSION_IMPL *session)
{
    WT_HAZARD *nhazard;
    size_t size;
    uint64_t hazard_gen;
    void *ohazard;

    /*
     * Allocate a new, larger hazard pointer array and copy the contents of the original into place.
     */
    size = session->hazard_size;
    WT_RET(__wt_calloc_def(session, size * 2, &nhazard));
    memcpy(nhazard, session->hazard, size * sizeof(WT_HAZARD));

    /*
     * Swap the new hazard pointer array into place after initialization is complete (initialization
     * must complete before eviction can see the new hazard pointer array), then schedule the
     * original to be freed.
     */
    ohazard = session->hazard;
    WT_PUBLISH(session->hazard, nhazard);

    /*
     * Increase the size of the session's pointer array after swapping it into place (the session's
     * reference must be updated before eviction can see the new size).
     */
    WT_PUBLISH(session->hazard_size, (uint32_t)(size * 2));

    /*
     * Threads using the hazard pointer array from now on will use the new one. Increment the hazard
     * pointer generation number, and schedule a future free of the old memory. Ignore any failure,
     * leak the memory.
     */
    __wt_gen_next(session, WT_GEN_HAZARD, &hazard_gen);
    WT_IGNORE_RET(
      __wt_stash_add(session, WT_GEN_HAZARD, hazard_gen, ohazard, size * sizeof(WT_HAZARD)));

    return (0);
}

/*
 * __wt_hazard_set_func --
 *     Set a hazard pointer.
 */
int
__wt_hazard_set_func(WT_SESSION_IMPL *session, WT_REF *ref, bool *busyp
#ifdef HAVE_DIAGNOSTIC
  ,
  const char *func, int line
#endif
)
{
    WT_HAZARD *hp;
    uint8_t current_state;

    *busyp = false;

    /* If a file can never be evicted, hazard pointers aren't required. */
    if (F_ISSET(S2BT(session), WT_BTREE_IN_MEMORY))
        return (0);

    /*
     * If there isn't a valid page, we're done. This read can race with eviction and splits, we
     * re-check it after a barrier to make sure we have a valid reference.
     */
    current_state = ref->state;
    if (current_state != WT_REF_MEM) {
        *busyp = true;
        return (0);
    }

    /* If we have filled the current hazard pointer array, grow it. */
    if (session->nhazard >= session->hazard_size) {
        WT_ASSERT(session,
          session->nhazard == session->hazard_size &&
            session->hazard_inuse == session->hazard_size);
        WT_RET(hazard_grow(session));
    }

    /*
     * If there are no available hazard pointer slots, make another one visible.
     */
    if (session->nhazard >= session->hazard_inuse) {
        WT_ASSERT(session,
          session->nhazard == session->hazard_inuse &&
            session->hazard_inuse < session->hazard_size);
        hp = &session->hazard[session->hazard_inuse++];
    } else {
        WT_ASSERT(session,
          session->nhazard < session->hazard_inuse &&
            session->hazard_inuse <= session->hazard_size);

        /*
         * There must be an empty slot in the array, find it. Skip most of the active slots by
         * starting after the active count slot; there may be a free slot before there, but checking
         * is expensive. If we reach the end of the array, continue the search from the beginning of
         * the array.
         */
        for (hp = session->hazard + session->nhazard;; ++hp) {
            if (hp >= session->hazard + session->hazard_inuse)
                hp = session->hazard;
            if (hp->ref == NULL)
                break;
        }
    }

    WT_ASSERT(session, hp->ref == NULL);

    /*
     * Do the dance:
     *
     * The memory location which makes a page "real" is the WT_REF's state of WT_REF_MEM, which can
     * be set to WT_REF_LOCKED at any time by the page eviction server.
     *
     * Add the WT_REF reference to the session's hazard list and flush the write, then see if the
     * page's state is still valid. If so, we can use the page because the page eviction server will
     * see our hazard pointer before it discards the page (the eviction server sets the state to
     * WT_REF_LOCKED, then flushes memory and checks the hazard pointers).
     */
    hp->ref = ref;
#ifdef HAVE_DIAGNOSTIC
    hp->func = func;
    hp->line = line;
#endif
    /* Publish the hazard pointer before reading page's state. */
    WT_FULL_BARRIER();

    /*
     * Check if the page state is still valid, where valid means a state of WT_REF_MEM.
     */
    current_state = ref->state;
    if (current_state == WT_REF_MEM) {
        ++session->nhazard;

        /*
         * Callers require a barrier here so operations holding the hazard pointer see consistent
         * data.
         */
        WT_READ_BARRIER();
        return (0);
    }

    /*
     * The page isn't available, it's being considered for eviction (or being evicted, for all we
     * know). If the eviction server sees our hazard pointer before evicting the page, it will
     * return the page to use, no harm done, if it doesn't, it will go ahead and complete the
     * eviction.
     *
     * We don't bother publishing this update: the worst case is we prevent some random page from
     * being evicted.
     */
    hp->ref = NULL;
    *busyp = true;
    return (0);
}

/*
 * __wt_hazard_clear --
 *     Clear a hazard pointer.
 */
int
__wt_hazard_clear(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_HAZARD *hp;

    /* If a file can never be evicted, hazard pointers aren't required. */
    if (F_ISSET(S2BT(session), WT_BTREE_IN_MEMORY))
        return (0);

    /*
     * Clear the caller's hazard pointer. The common pattern is LIFO, so do a reverse search.
     */
    for (hp = session->hazard + session->hazard_inuse - 1; hp >= session->hazard; --hp)
        if (hp->ref == ref) {
            /*
             * We don't publish the hazard pointer clear in the general case. It's not required for
             * correctness; it gives an eviction thread faster access to the page were the page
             * selected for eviction.
             */
            hp->ref = NULL;

            /*
             * If this was the last hazard pointer in the session, reset the size so that checks can
             * skip this session.
             *
             * A write-barrier() is necessary before the change to the in-use value, the number of
             * active references can never be less than the number of in-use slots.
             */
            if (--session->nhazard == 0)
                WT_PUBLISH(session->hazard_inuse, 0);
            return (0);
        }

    /*
     * A serious error, we should always find the hazard pointer. Panic, because using a page we
     * didn't have pinned down implies corruption.
     */
    WT_RET_PANIC(session, EINVAL, "session %p: clear hazard pointer: %p: not found",
      (void *)session, (void *)ref);
}

/*
 * __wt_hazard_close --
 *     Verify that no hazard pointers are set.
 */
void
__wt_hazard_close(WT_SESSION_IMPL *session)
{
    WT_HAZARD *hp;
    bool found;

    /*
     * Check for a set hazard pointer and complain if we find one. We could just check the session's
     * hazard pointer count, but this is a useful diagnostic.
     */
    for (found = false, hp = session->hazard; hp < session->hazard + session->hazard_inuse; ++hp)
        if (hp->ref != NULL) {
            found = true;
            break;
        }
    if (session->nhazard == 0 && !found)
        goto weak;

    __wt_errx(session, "session %p: close hazard pointer table: table not empty", (void *)session);

#ifdef HAVE_DIAGNOSTIC
    __hazard_dump(session);
#endif

    /*
     * Clear any hazard pointers because it's not a correctness problem (any hazard pointer we find
     * can't be real because the session is being closed when we're called). We do this work because
     * session close isn't that common that it's an expensive check, and we don't want to let a
     * hazard pointer lie around, keeping a page from being evicted.
     *
     * We don't panic: this shouldn't be a correctness issue (at least, I can't think of a reason it
     * would be).
     */
    for (hp = session->hazard; hp < session->hazard + session->hazard_inuse; ++hp)
        if (hp->ref != NULL) {
            hp->ref = NULL;
            --session->nhazard;
        }

    if (session->nhazard != 0)
        __wt_errx(session, "session %p: close hazard pointer table: count didn't match entries",
          (void *)session);

weak:
    /* Same for weak hazard pointers. */
    __wt_hazard_weak_close(session);
}

/*
 * hazard_get_reference --
 *     Return a consistent reference to a hazard pointer array.
 */
static inline void
hazard_get_reference(WT_SESSION_IMPL *session, WT_HAZARD **hazardp, uint32_t *hazard_inusep)
{
    /*
     * Hazard pointer arrays can be swapped out from under us if they grow. First, read the current
     * in-use value. The read must precede the read of the hazard pointer itself (so the in-use
     * value is pessimistic should the hazard array grow), and additionally ensure we only read the
     * in-use value once. Then, read the hazard pointer, also ensuring we only read it once.
     *
     * Use a barrier instead of marking the fields volatile because we don't want to slow down the
     * rest of the hazard pointer functions that don't need special treatment.
     */
    WT_ORDERED_READ(*hazard_inusep, session->hazard_inuse);
    WT_ORDERED_READ(*hazardp, session->hazard);
}

/*
 * __wt_hazard_check --
 *     Return if there's a hazard pointer to the page in the system.
 */
WT_HAZARD *
__wt_hazard_check(WT_SESSION_IMPL *session, WT_REF *ref, WT_SESSION_IMPL **sessionp)
{
    WT_CONNECTION_IMPL *conn;
    WT_HAZARD *hp;
    WT_SESSION_IMPL *s;
    uint32_t i, j, hazard_inuse, max, session_cnt, walk_cnt;

    /* If a file can never be evicted, hazard pointers aren't required. */
    if (F_ISSET(S2BT(session), WT_BTREE_IN_MEMORY))
        return (NULL);

    conn = S2C(session);

    WT_STAT_CONN_INCR(session, cache_hazard_checks);

    /*
     * Hazard pointer arrays might grow and be freed underneath us; enter the current hazard
     * resource generation for the duration of the walk to ensure that doesn't happen.
     */
    __wt_session_gen_enter(session, WT_GEN_HAZARD);

    /*
     * No lock is required because the session array is fixed size, but it may contain inactive
     * entries. We must review any active session that might contain a hazard pointer, so insert a
     * read barrier after reading the active session count. That way, no matter what sessions come
     * or go, we'll check the slots for all of the sessions that could have been active when we
     * started our check.
     */
    WT_ORDERED_READ(session_cnt, conn->session_cnt);
    for (s = conn->sessions, i = max = walk_cnt = 0; i < session_cnt; ++s, ++i) {
        if (!s->active)
            continue;

        hazard_get_reference(s, &hp, &hazard_inuse);

        if (hazard_inuse > max) {
            max = hazard_inuse;
            WT_STAT_CONN_SET(session, cache_hazard_max, max);
        }

        for (j = 0; j < hazard_inuse; ++hp, ++j) {
            ++walk_cnt;
            if (hp->ref == ref) {
                WT_STAT_CONN_INCRV(session, cache_hazard_walks, walk_cnt);
                if (sessionp != NULL)
                    *sessionp = s;
                goto done;
            }
        }
    }
    WT_STAT_CONN_INCRV(session, cache_hazard_walks, walk_cnt);
    hp = NULL;

done:
    /* Leave the current resource generation. */
    __wt_session_gen_leave(session, WT_GEN_HAZARD);

    return (hp);
}

/*
 * __wt_hazard_count --
 *     Count how many hazard pointers this session has on the given page.
 */
u_int
__wt_hazard_count(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_HAZARD *hp;
    uint32_t i, hazard_inuse;
    u_int count;

    hp = session->hazard;
    hazard_inuse = session->hazard_inuse;

    for (count = 0, i = 0; i < hazard_inuse; ++hp, ++i)
        if (hp->ref == ref)
            ++count;

    return (count);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_hazard_check_assert --
 *     Assert there's no hazard pointer to the page.
 */
bool
__wt_hazard_check_assert(WT_SESSION_IMPL *session, void *ref, bool waitfor)
{
    WT_HAZARD *hp;
    WT_SESSION_IMPL *s;
    int i;

    s = NULL;
    for (i = 0;;) {
        if ((hp = __wt_hazard_check(session, ref, &s)) == NULL)
            return (true);
        if (!waitfor || ++i > 100)
            break;
        __wt_sleep(0, 10000);
    }
    __wt_errx(session,
      "hazard pointer reference to discarded object: (%p: session %p name %s: %s, line %d)",
      (void *)hp->ref, (void *)s, s->name == NULL ? "UNKNOWN" : s->name, hp->func, hp->line);
    return (false);
}

/*
 * __hazard_dump --
 *     Display the list of hazard pointers.
 */
static void
__hazard_dump(WT_SESSION_IMPL *session)
{
    WT_HAZARD *hp;

    for (hp = session->hazard; hp < session->hazard + session->hazard_inuse; ++hp)
        if (hp->ref != NULL)
            __wt_errx(session, "session %p: hazard pointer %p: %s, line %d", (void *)session,
              (void *)hp->ref, hp->func, hp->line);
}
#endif
