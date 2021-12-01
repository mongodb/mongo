/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define WT_HAZARD_WEAK_FORALL(s, wha, whp)                     \
    for (wha = (s)->hazard_weak; wha != NULL; wha = wha->next) \
        for (whp = wha->hazard; whp < wha->hazard + wha->hazard_inuse; whp++)

#define WT_HAZARD_WEAK_FORALL_BARRIER(s, wha, whp)               \
    for (wha = (s)->hazard_weak; wha != NULL; wha = wha->next) { \
        uint32_t __hazard_inuse;                                 \
        WT_ORDERED_READ(__hazard_inuse, wha->hazard_inuse);      \
        for (whp = wha->hazard; whp < wha->hazard + __hazard_inuse; whp++)

#define WT_HAZARD_WEAK_FORALL_BARRIER_END }

/*
 * __wt_hazard_weak_close --
 *     Verify that no weak hazard pointers are set.
 */
void
__wt_hazard_weak_close(WT_SESSION_IMPL *session)
{
    WT_HAZARD_WEAK *whp;
    WT_HAZARD_WEAK_ARRAY *wha;
    uint32_t nhazard_weak;
    bool found;

    /*
     * Check for a set weak hazard pointer and complain if we find one. We could just check the
     * session's weak hazard pointer count, but this is a useful diagnostic.
     */
    for (found = false, nhazard_weak = 0, wha = session->hazard_weak; wha != NULL;
         nhazard_weak += wha->nhazard, wha = wha->next)
        for (whp = wha->hazard; whp < wha->hazard + wha->hazard_inuse; whp++)
            if (whp->ref != NULL) {
                found = true;
                break;
            }

    if (nhazard_weak == 0 && !found)
        return;

    __wt_errx(
      session, "session %p: close weak hazard pointer table: table not empty", (void *)session);

    WT_HAZARD_WEAK_FORALL (session, wha, whp)
        if (whp->ref != NULL) {
            whp->ref = NULL;
            --wha->nhazard;
            --nhazard_weak;
        }

    if (nhazard_weak != 0)
        __wt_errx(session,
          "session %p: close weak hazard pointer table: count didn't match entries",
          (void *)session);
}

/*
 * hazard_weak_grow --
 *     Grow a weak hazard pointer array. What we have is a list of arrays doubling in size, with the
 *     largest array being at the head of the list. The array at the head of the list is the only
 *     one we actively use to set the new weak hazard pointers. The older arrays get emptied as the
 *     sessions using them resolve their transactions, eventually leaving them all empty.
 */
static int
hazard_weak_grow(WT_SESSION_IMPL *session)
{
    WT_HAZARD_WEAK_ARRAY *wha;
    size_t size;

    /*
     * Allocate a new, larger hazard pointer array and link it into place.
     */
    size = session->hazard_weak->hazard_size;
    WT_RET(__wt_calloc(
      session, sizeof(WT_HAZARD_WEAK_ARRAY) + 2 * size * sizeof(WT_HAZARD_WEAK), 1, &wha));
    wha->next = session->hazard_weak;
    wha->hazard_size = (uint32_t)(size * 2);

    /*
     * Swap the new hazard pointer array into place after initialization is complete (initialization
     * must complete before eviction can see the new hazard pointer array).
     */
    WT_PUBLISH(session->hazard_weak, wha);

    return (0);
}

/*
 * __wt_hazard_weak_destroy --
 *     Free all memory associated with weak hazard pointers
 */
void
__wt_hazard_weak_destroy(WT_SESSION_IMPL *session_safe, WT_SESSION_IMPL *s)
{
    WT_HAZARD_WEAK_ARRAY *wha, *next;

    for (wha = s->hazard_weak; wha != NULL; wha = next) {
        next = wha->next;
        __wt_free(session_safe, wha);
    }
}

/*
 * __wt_hazard_weak_set --
 *     Set a weak hazard pointer. A hazard pointer must be held on the ref.
 */
int
__wt_hazard_weak_set(WT_SESSION_IMPL *session, WT_REF *ref, WT_TXN_OP *op)
{
    WT_HAZARD_WEAK *whp;
    WT_HAZARD_WEAK_ARRAY *wha;

    WT_ASSERT(session, ref != NULL);
    WT_ASSERT(session, op->whp == NULL);

    /* If a file can never be evicted, hazard pointers aren't required. */
    if (F_ISSET(op->btree, WT_BTREE_IN_MEMORY))
        return (0);

    /* If we have filled the current hazard pointer array, grow it. */
    for (wha = session->hazard_weak; wha != NULL && wha->nhazard >= wha->hazard_size;
         wha = wha->next)
        WT_ASSERT(
          session, wha->nhazard == wha->hazard_size && wha->hazard_inuse == wha->hazard_size);

    if (wha == NULL) {
        WT_RET(hazard_weak_grow(session));
        wha = session->hazard_weak;
    }

    /*
     * If there are no available hazard pointer slots, make another one visible.
     */
    if (wha->nhazard >= wha->hazard_inuse) {
        WT_ASSERT(
          session, wha->nhazard == wha->hazard_inuse && wha->hazard_inuse < wha->hazard_size);
        whp = &wha->hazard[wha->hazard_inuse++];
    } else {
        WT_ASSERT(
          session, wha->nhazard < wha->hazard_inuse && wha->hazard_inuse <= wha->hazard_size);

        /*
         * There must be an empty slot in the array, find it. Skip most of the active slots by
         * starting after the active count slot; there may be a free slot before there, but checking
         * is expensive. If we reach the end of the array, continue the search from the beginning of
         * the array.
         */
        for (whp = wha->hazard + wha->nhazard;; ++whp) {
            if (whp >= wha->hazard + wha->hazard_inuse)
                whp = wha->hazard;
            if (whp->ref == NULL)
                break;
        }
    }

    ++wha->nhazard;

    WT_ASSERT(session, whp->ref == NULL);

    /*
     * We rely on a hazard pointer protecting the ref, so for weak hazard pointers this is much
     * simpler than the regular hazard pointer case.
     */
    whp->ref = ref;
    whp->valid = true;

    op->whp = whp;
    return (0);
}

/*
 * __wt_hazard_weak_clear --
 *     Clear a weak hazard pointer.
 */
int
__wt_hazard_weak_clear(WT_SESSION_IMPL *session, WT_HAZARD_WEAK **whpp)
{
    WT_HAZARD_WEAK *whp;
    WT_HAZARD_WEAK_ARRAY *wha;

    whp = *whpp;
    *whpp = NULL;

    /*
     * We don't publish the weak hazard pointer clear as we only clear while holding the hazard
     * pointer to the page, preventing eviction from looking for this weak pointer.
     */
    whp->ref = NULL;

    /*
     * Find the array this hazard pointer belongs to, and do the accounting for using one less slot.
     */
    for (wha = session->hazard_weak; wha != NULL; wha = wha->next) {
        if (whp >= wha->hazard && whp < wha->hazard + wha->hazard_size) {
            if (wha->nhazard == 0)
                WT_RET_PANIC(session, EINVAL,
                  "session %p: While clearing weak hazard pointer, the count of the pointers "
                  "went negative for the relevant array.",
                  (void *)session);
            if (--wha->nhazard == 0)
                WT_PUBLISH(wha->hazard_inuse, 0);
            break;
        }
    }
    /*
     * We should always be able to find the array. Panic, because we messed up and it could imply
     * corruption.
     */
    if (wha == NULL)
        WT_RET_PANIC(session, EINVAL,
          "session %p: While clearing weak hazard pointer could not find the array.",
          (void *)session);

    return (0);
}

/*
 * __wt_hazard_weak_invalidate --
 *     Invalidate any weak hazard pointers on a page that is locked for eviction.
 */
void
__wt_hazard_weak_invalidate(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_CONNECTION_IMPL *conn;
    WT_HAZARD_WEAK *whp;
    WT_HAZARD_WEAK_ARRAY *wha;
    WT_SESSION_IMPL *s;
    uint32_t i, session_cnt, walk_cnt;

    /* If a file can never be evicted, hazard pointers aren't required. */
    if (F_ISSET(S2BT(session), WT_BTREE_IN_MEMORY))
        return;

    conn = S2C(session);

    /*
     * No lock is required because the session array is fixed size, but it may contain inactive
     * entries. We must review any active session that might contain a hazard pointer, so insert a
     * read barrier after reading the active session count. That way, no matter what sessions come
     * or go, we'll check the slots for all of the sessions that could have been active when we
     * started our check.
     */
    WT_ORDERED_READ(session_cnt, conn->session_cnt);
    for (s = conn->sessions, i = walk_cnt = 0; i < session_cnt; ++s, ++i) {
        if (!s->active)
            continue;

        WT_HAZARD_WEAK_FORALL_BARRIER(s, wha, whp)
        {
            ++walk_cnt;
            if (whp->ref == ref)
                whp->valid = false;
        }
        WT_HAZARD_WEAK_FORALL_BARRIER_END
    }
    WT_STAT_CONN_INCRV(session, cache_hazard_walks, walk_cnt);
}

/*
 * __wt_hazard_weak_upgrade --
 *     Attempts to convert a weak hazard pointer into a full hazard pointer, failing if it has been
 *     invalidated.
 */
int
__wt_hazard_weak_upgrade(WT_SESSION_IMPL *session, WT_HAZARD_WEAK **whpp, WT_REF **refp)
{
    WT_DECL_RET;
    WT_HAZARD_WEAK *whp;
    bool busy;

    *refp = NULL;
    whp = *whpp;

    /* If a file can never be evicted, hazard pointers aren't required. */
    if (F_ISSET(S2BT(session), WT_BTREE_IN_MEMORY))
        return (0);

    /*
     * An empty slot reflects a serious error, we should always find the weak hazard pointer.
     * Assert, because we messed up in and it could imply corruption.
     */
    WT_ASSERT(session, whp != NULL && whp->ref != NULL);

    /* If the weak pointer has already been invalidated, we can't upgrade, we are done. */
    if (!whp->valid)
        WT_ERR(EBUSY);

#ifdef HAVE_DIAGNOSTIC
    /*
     * Failing to upgrade the hazard pointer will encourage testing the resolution of uncommitted
     * updates more often.
     */
    if (__wt_random(&session->rnd) % 10 == 0)
        WT_ERR(EBUSY);
#endif

    /*
     * Attempt to take a strong hazard pointer. Eviction on this page might prevent us from being
     * able to do so, in such a case we can't upgrade, we are done.
     */
    WT_ERR(__wt_hazard_set(session, whp->ref, &busy));
    if (busy)
        WT_ERR(EBUSY);

    /*
     * Paranoia: Eviction could still race with us and mark the pointers invalid after we have
     * checked their validity and before setting strong hazard pointer. Check again.
     */
    if (!whp->valid) {
        WT_ERR(__wt_hazard_clear(session, whp->ref));
        WT_ERR(EBUSY);
    }

    /*
     * We have successfully upgraded. Clear the weak hazards and return the page reference. We want
     * to clear the weak hazard pointers even in case of errors as we will take a slow path to
     * resolve the updates.
     */
    *refp = whp->ref;

err:
    WT_TRET(__wt_hazard_weak_clear(session, whpp));
    return (ret);
}
