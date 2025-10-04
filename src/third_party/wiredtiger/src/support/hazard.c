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
    WT_HAZARD *new_hazard;
    size_t size;
    uint64_t hazard_gen;
    void *old_hazard;

    /*
     * Allocate a new, larger hazard pointer array and copy the contents of the original into place.
     */
    size = session->hazards.size;
    WT_RET(__wt_calloc_def(session, size * 2, &new_hazard));
    memcpy(new_hazard, session->hazards.arr, size * sizeof(WT_HAZARD));

    /*
     * Swap the new hazard pointer array into place after initialization is complete (initialization
     * must complete before eviction can see the new hazard pointer array), then schedule the
     * original to be freed.
     */
    old_hazard = session->hazards.arr;
    WT_RELEASE_WRITE_WITH_BARRIER(session->hazards.arr, new_hazard);

    /*
     * Our larger hazard array means we can use larger indices for reading/writing hazard pointers.
     * However, if these larger indices become visible to other threads before the new hazard array
     * we can have out of bounds accesses to the old hazard array. Set a release barrier here to
     * ensure the array pointer is always visible first.
     */
    WT_RELEASE_BARRIER();

    session->hazards.size = (uint32_t)(size * 2);

    /*
     * Threads using the hazard pointer array from now on will use the new one. Increment the hazard
     * pointer generation number, and schedule a future free of the old memory. Ignore any failure,
     * leak the memory.
     */
    __wt_gen_next(session, WT_GEN_HAZARD, &hazard_gen);
    WT_IGNORE_RET(__wt_stash_add(session, WT_GEN_HAZARD, hazard_gen, old_hazard, 0));

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
    WT_REF_STATE current_state;

    *busyp = false;

    /* If a file can never be evicted, hazard pointers aren't required. */
    if (F_ISSET(S2BT(session), WT_BTREE_NO_EVICT))
        return (0);

    /*
     * If there isn't a valid page, we're done. This read can race with eviction and splits, we
     * re-check it after a barrier to make sure we have a valid reference.
     */
    current_state = WT_REF_GET_STATE(ref);
    if (current_state != WT_REF_MEM) {
        *busyp = true;
        return (0);
    }

    /* If we have filled the current hazard pointer array, grow it. */
    if (session->hazards.num_active >= session->hazards.size) {
        WT_ASSERT(session,
          session->hazards.num_active == session->hazards.size &&
            __wt_atomic_load32(&session->hazards.inuse) == session->hazards.size);
        WT_RET(hazard_grow(session));
    }

    /*
     * If there are no available hazard pointer slots, make another one visible.
     */
    if (session->hazards.num_active >= __wt_atomic_load32(&session->hazards.inuse)) {
        WT_ASSERT(session,
          session->hazards.num_active == __wt_atomic_load32(&session->hazards.inuse) &&
            __wt_atomic_load32(&session->hazards.inuse) < session->hazards.size);
        /*
         * If we've grown the hazard array the inuse counter can be incremented beyond the size of
         * the old hazard array. We need to ensure the new hazard array pointer is visible before
         * this increment of the inuse counter and do so with a release barrier in the hazard grow
         * logic.
         */
        hp = &session->hazards.arr[__wt_atomic_fetch_add32(&session->hazards.inuse, 1)];
    } else {
        WT_ASSERT(session,
          session->hazards.num_active < __wt_atomic_load32(&session->hazards.inuse) &&
            __wt_atomic_load32(&session->hazards.inuse) <= session->hazards.size);

        /*
         * There must be an empty slot in the array, find it. Skip most of the active slots by
         * starting after the active count slot; there may be a free slot before there, but checking
         * is expensive. If we reach the end of the array, continue the search from the beginning of
         * the array.
         */
        for (hp = session->hazards.arr + session->hazards.num_active;; ++hp) {
            if (hp >= session->hazards.arr + __wt_atomic_load32(&session->hazards.inuse))
                hp = session->hazards.arr;
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
    current_state = WT_REF_GET_STATE(ref);
    if (current_state == WT_REF_MEM) {
        ++session->hazards.num_active;

        /*
         * Callers require a barrier here so operations holding the hazard pointer see consistent
         * data.
         */
        WT_ACQUIRE_BARRIER();
        return (0);
    }

    /*
     * The page isn't available, it's being considered for eviction (or being evicted, for all we
     * know). If the eviction server sees our hazard pointer before evicting the page, it will
     * return the page to use, no harm done, if it doesn't, it will go ahead and complete the
     * eviction.
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
    if (F_ISSET(S2BT(session), WT_BTREE_NO_EVICT))
        return (0);

    /*
     * Clear the caller's hazard pointer. The common pattern is LIFO, so do a reverse search.
     */
    for (hp = session->hazards.arr + __wt_atomic_load32(&session->hazards.inuse) - 1;
         hp >= session->hazards.arr; --hp)
        if (hp->ref == ref) {
            /*
             * Release write the hazard pointer. We want to ensure that all operations performed on
             * the page, be it writes or reads, occur while we are holding the hazard pointer and
             * thus preventing the page from being freed.
             */
            WT_RELEASE_WRITE(hp->ref, NULL);

            /*
             * If this was the last hazard pointer in the session, reset the size so that checks can
             * skip this session.
             *
             * A write-barrier() is necessary before the change to the in-use value, the number of
             * active references can never be less than the number of in-use slots.
             */
            if (--session->hazards.num_active == 0)
                WT_RELEASE_WRITE_WITH_BARRIER(session->hazards.inuse, 0);
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
    for (found = false, hp = session->hazards.arr;
         hp < session->hazards.arr + __wt_atomic_load32(&session->hazards.inuse); ++hp)
        if (hp->ref != NULL) {
            found = true;
            break;
        }
    if (session->hazards.num_active == 0 && !found)
        return;

    __wt_errx(session, "session %p: close hazard pointer table: table not empty", (void *)session);

#ifdef HAVE_DIAGNOSTIC
    __hazard_dump(session);
    WT_ASSERT(session, session->hazards.num_active == 0 && !found);
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
    for (hp = session->hazards.arr;
         hp < session->hazards.arr + __wt_atomic_load32(&session->hazards.inuse); ++hp)
        if (hp->ref != NULL) {
            hp->ref = NULL;
            --session->hazards.num_active;
        }

    if (session->hazards.num_active != 0)
        __wt_errx(session, "session %p: close hazard pointer table: count didn't match entries",
          (void *)session);
}

/*
 * hazard_get_reference --
 *     Return a consistent reference to a hazard pointer array.
 */
static WT_INLINE void
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
    WT_ACQUIRE_READ_WITH_BARRIER(*hazard_inusep, session->hazards.inuse);
    WT_ACQUIRE_READ_WITH_BARRIER(*hazardp, session->hazards.arr);
}

/*
 * __hazard_check_callback --
 *     Check if a session holds a hazard pointer on a given ref. If it does return both the session
 *     and the hazard pointer. Callback from the session array walk.
 */
static int
__hazard_check_callback(
  WT_SESSION_IMPL *session, WT_SESSION_IMPL *array_session, bool *exit_walkp, void *cookiep)
{
    WT_HAZARD_COOKIE *cookie;
    uint32_t hazard_inuse, i;

    cookie = (WT_HAZARD_COOKIE *)cookiep;
    hazard_get_reference(array_session, &cookie->ret_hp, &hazard_inuse);

    if (hazard_inuse > cookie->max) {
        cookie->max = hazard_inuse;
        WT_STAT_CONN_SET(session, cache_hazard_max, cookie->max);
    }

    for (i = 0; i < hazard_inuse; ++cookie->ret_hp, ++i) {
        ++cookie->walk_cnt;
        if (cookie->ret_hp->ref == cookie->search_ref) {
            WT_STAT_CONN_INCRV(session, cache_hazard_walks, cookie->walk_cnt);
            if (cookie->ret_session != NULL)
                *cookie->ret_session = array_session;
            *exit_walkp = true;
            return (0);
        }
    }

    /*
     * We didn't find a hazard pointer. Clear this field so we don't accidentally report the last
     * iterated hazard pointer
     */
    cookie->ret_hp = NULL;
    return (0);
}

/*
 * __wt_hazard_check --
 *     Return if there's a hazard pointer to the page in the system.
 */
WT_HAZARD *
__wt_hazard_check(WT_SESSION_IMPL *session, WT_REF *ref, WT_SESSION_IMPL **sessionp)
{
    WT_HAZARD_COOKIE cookie;

    WT_CLEAR(cookie);
    cookie.ret_session = sessionp;
    cookie.search_ref = ref;

    /* If a file can never be evicted, hazard pointers aren't required. */
    if (F_ISSET(S2BT(session), WT_BTREE_NO_EVICT))
        return (NULL);

    WT_STAT_CONN_INCR(session, cache_hazard_checks);
    /*
     * Hazard pointer arrays might grow and be freed underneath us; enter the current hazard
     * resource generation for the duration of the walk to ensure that doesn't happen.
     */
    __wt_session_gen_enter(session, WT_GEN_HAZARD);
    WT_IGNORE_RET(__wt_session_array_walk(session, __hazard_check_callback, false, &cookie));

    if (cookie.ret_hp == NULL)
        /*
         * We increment this stat inside the walk logic when we find a hazard pointer. Since we
         * didn't find one increment here instead.
         */
        WT_STAT_CONN_INCRV(session, cache_hazard_walks, cookie.walk_cnt);

    /* Leave the current resource generation. */
    __wt_session_gen_leave(session, WT_GEN_HAZARD);

    return (cookie.ret_hp);
}

/*
 * __wt_hazard_count --
 *     Count how many hazard pointers this session has on the given page.
 */
u_int
__wt_hazard_count(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_HAZARD *hp;
    uint32_t hazard_inuse, i;
    u_int count;

    hazard_get_reference(session, &hp, &hazard_inuse);

    for (count = 0, i = 0; i < hazard_inuse; ++hp, ++i)
        if (hp->ref == ref)
            ++count;

    return (count);
}

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
        __wt_sleep(0, 10 * WT_THOUSAND);
    }
#ifdef HAVE_DIAGNOSTIC
    /*
     * In diagnostic mode we also track the file and line where the hazard pointer is set. If this
     * is available report it in the error trace.
     */
    __wt_errx(session,
      "hazard pointer reference to discarded object: (%p: session %p name %s: %s, line %d)",
      (void *)hp->ref, (void *)s, s->name == NULL ? "UNKNOWN" : s->name, hp->func, hp->line);
#else
    __wt_errx(session, "hazard pointer reference to discarded object: (%p: session %p name %s)",
      (void *)hp->ref, (void *)s, s->name == NULL ? "UNKNOWN" : s->name);
#endif
    return (false);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __hazard_dump --
 *     Display the list of hazard pointers.
 */
static void
__hazard_dump(WT_SESSION_IMPL *session)
{
    WT_HAZARD *hp;

    for (hp = session->hazards.arr;
         hp < session->hazards.arr + __wt_atomic_load32(&session->hazards.inuse); ++hp)
        if (hp->ref != NULL)
            __wt_errx(session, "session %p: hazard pointer %p: %s, line %d", (void *)session,
              (void *)hp->ref, hp->func, hp->line);
}
#endif
