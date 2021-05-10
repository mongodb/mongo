/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rec_dictionary_skip_search --
 *     Search a dictionary skiplist.
 */
static WT_REC_DICTIONARY *
__rec_dictionary_skip_search(WT_REC_DICTIONARY **head, uint64_t hash)
{
    WT_REC_DICTIONARY **e;
    int i;

    /*
     * Start at the highest skip level, then go as far as possible at each level before stepping
     * down to the next.
     */
    for (i = WT_SKIP_MAXDEPTH - 1, e = &head[i]; i >= 0;) {
        if (*e == NULL) { /* Empty levels */
            --i;
            --e;
            continue;
        }

        /*
         * Return any exact matches: we don't care in what search level we found a match.
         */
        if ((*e)->hash == hash) /* Exact match */
            return (*e);
        if ((*e)->hash > hash) { /* Drop down a level */
            --i;
            --e;
        } else /* Keep going at this level */
            e = &(*e)->next[i];
    }
    return (NULL);
}

/*
 * __rec_dictionary_skip_search_stack --
 *     Search a dictionary skiplist, returning an insert/remove stack.
 */
static void
__rec_dictionary_skip_search_stack(
  WT_REC_DICTIONARY **head, WT_REC_DICTIONARY ***stack, uint64_t hash)
{
    WT_REC_DICTIONARY **e;
    int i;

    /*
     * Start at the highest skip level, then go as far as possible at each level before stepping
     * down to the next.
     */
    for (i = WT_SKIP_MAXDEPTH - 1, e = &head[i]; i >= 0;)
        if (*e == NULL || (*e)->hash > hash)
            stack[i--] = e--; /* Drop down a level */
        else
            e = &(*e)->next[i]; /* Keep going at this level */
}

/*
 * __rec_dictionary_skip_insert --
 *     Insert an entry into the dictionary skip-list.
 */
static void
__rec_dictionary_skip_insert(WT_REC_DICTIONARY **head, WT_REC_DICTIONARY *e, uint64_t hash)
{
    WT_REC_DICTIONARY **stack[WT_SKIP_MAXDEPTH];
    u_int i;

    /* Insert the new entry into the skiplist. */
    __rec_dictionary_skip_search_stack(head, stack, hash);
    for (i = 0; i < e->depth; ++i) {
        e->next[i] = *stack[i];
        *stack[i] = e;
    }
}

/*
 * __wt_rec_dictionary_init --
 *     Allocate and initialize the dictionary.
 */
int
__wt_rec_dictionary_init(WT_SESSION_IMPL *session, WT_RECONCILE *r, u_int slots)
{
    u_int depth, i;

    /* Free any previous dictionary. */
    __wt_rec_dictionary_free(session, r);

    r->dictionary_slots = slots;
    WT_RET(__wt_calloc(session, r->dictionary_slots, sizeof(WT_REC_DICTIONARY *), &r->dictionary));
    for (i = 0; i < r->dictionary_slots; ++i) {
        depth = __wt_skip_choose_depth(session);
        WT_RET(__wt_calloc(session, 1,
          sizeof(WT_REC_DICTIONARY) + depth * sizeof(WT_REC_DICTIONARY *), &r->dictionary[i]));
        r->dictionary[i]->depth = depth;
    }
    return (0);
}

/*
 * __wt_rec_dictionary_free --
 *     Free the dictionary.
 */
void
__wt_rec_dictionary_free(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
    u_int i;

    if (r->dictionary == NULL)
        return;

    /*
     * We don't correct dictionary_slots when we fail during allocation, but that's OK, the value is
     * either NULL or a memory reference to be free'd.
     */
    for (i = 0; i < r->dictionary_slots; ++i)
        __wt_free(session, r->dictionary[i]);
    __wt_free(session, r->dictionary);
}

/*
 * __wt_rec_dictionary_reset --
 *     Reset the dictionary when reconciliation restarts and when crossing a page boundary (a
 *     potential split).
 */
void
__wt_rec_dictionary_reset(WT_RECONCILE *r)
{
    if (r->dictionary_slots) {
        r->dictionary_next = 0;
        memset(r->dictionary_head, 0, sizeof(r->dictionary_head));
    }
}

/*
 * __wt_rec_dictionary_lookup --
 *     Check the dictionary for a matching value on this page.
 */
int
__wt_rec_dictionary_lookup(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REC_KV *val, WT_REC_DICTIONARY **dpp)
{
    WT_REC_DICTIONARY *dp, *next;
    uint64_t hash;
    bool match;

    *dpp = NULL;

    /* Search the dictionary, and return any match we find. */
    hash = __wt_hash_city64(val->buf.data, val->buf.size);
    for (dp = __rec_dictionary_skip_search(r->dictionary_head, hash);
         dp != NULL && dp->hash == hash; dp = dp->next[0]) {
        WT_RET(
          __wt_cell_pack_value_match((WT_CELL *)((uint8_t *)r->cur_ptr->image.mem + dp->offset),
            &val->cell, val->buf.data, &match));
        if (match) {
            WT_STAT_DATA_INCR(session, rec_dictionary);
            *dpp = dp;
            return (0);
        }
    }

    /*
     * We're not doing value replacement in the dictionary. We stop adding new entries if we run out
     * of empty dictionary slots (but continue to use the existing entries). I can't think of any
     * reason a leaf page value is more likely to be seen because it was seen more recently than
     * some other value: if we find working sets where that's not the case, it shouldn't be too
     * difficult to maintain a pointer which is the next dictionary slot to re-use.
     */
    if (r->dictionary_next >= r->dictionary_slots)
        return (0);

    /*
     * Set the hash value, we'll add this entry into the dictionary when we write it into the page's
     * disk image buffer (because that's when we know where on the page it will be written).
     */
    next = r->dictionary[r->dictionary_next++];
    next->offset = 0; /* Not necessary, just cautious. */
    next->hash = hash;
    __rec_dictionary_skip_insert(r->dictionary_head, next, hash);
    *dpp = next;
    return (0);
}
