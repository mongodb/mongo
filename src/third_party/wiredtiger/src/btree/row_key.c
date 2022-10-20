/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_row_leaf_key_copy --
 *     Get a copy of a row-store leaf-page key.
 */
int
__wt_row_leaf_key_copy(WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip, WT_ITEM *key)
{
    WT_RET(__wt_row_leaf_key(session, page, rip, key, false));

    /* The return buffer may only hold a reference to a key, copy it. */
    if (!WT_DATA_IN_ITEM(key))
        WT_RET(__wt_buf_set(session, key, key->data, key->size));

    return (0);
}

/*
 * __wt_row_leaf_key_work --
 *     Return a reference to a row-store leaf-page key, optionally instantiate the key into the
 *     in-memory page.
 */
int
__wt_row_leaf_key_work(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip_arg, WT_ITEM *keyb, bool instantiate)
{
    enum { FORWARD, BACKWARD } direction;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV *unpack, _unpack;
    WT_DECL_RET;
    WT_IKEY *ikey;
    WT_ROW *rip, *jump_rip;
    size_t group_size, key_size;
    uint32_t slot;
    u_int jump_slot_offset, slot_offset;
    uint8_t group_prefix, key_prefix, last_prefix;
    void *copy;
    const void *group_key, *key_data;

    /*
     * It is unusual to call this function: most code should be calling the front-end,
     * __wt_row_leaf_key, be careful if you're calling this code directly.
     */

    btree = S2BT(session);
    unpack = &_unpack;
    rip = rip_arg;

    jump_rip = NULL;
    jump_slot_offset = 0;
    last_prefix = key_prefix = 0;

    key_data = NULL; /* -Werror=maybe-uninitialized */
    key_size = 0;    /* -Werror=maybe-uninitialized */

    direction = BACKWARD;
    for (slot_offset = 0;;) {
        if (0) {
switch_and_jump:
            /* Switching to a forward roll. */
            WT_ASSERT(session, direction == BACKWARD);
            direction = FORWARD;

            /* Skip list of keys with compatible prefixes. */
            rip = jump_rip;
            slot_offset = jump_slot_offset;
        }

overflow_retry:
        /*
         * Figure out what the key looks like. The row-store key can change underfoot; explicitly
         * take a copy.
         */
        copy = WT_ROW_KEY_COPY(rip);
        __wt_row_leaf_key_info(page, copy, &ikey, &cell, &key_data, &key_size, &key_prefix);

        /* 1: the test for a directly referenced on-page key. */
        if (ikey == NULL && key_data != NULL) {
            /*
             * If there's a key without prefix compression, we're good to go, otherwise we have to
             * deal with the prefix.
             */
            if (key_prefix == 0) {
                keyb->data = key_data;
                keyb->size = key_size;
            } else
                goto prefix_continue;

            /*
             * If this is the key we originally wanted, we don't care if we're rolling forward or
             * backward, or if it's an overflow key or not, it's what we wanted. This shouldn't
             * normally happen, the fast-path code that front-ends this function will have figured
             * it out before we were called.
             *
             * The key doesn't need to be instantiated, just return.
             */
            if (slot_offset == 0)
                return (0);

            /*
             * This key is not an overflow key by definition and isn't compressed in any way, we can
             * use it to roll forward.
             *
             * If rolling backward, switch directions.
             *
             * If rolling forward: there's a bug somewhere, we should have hit this key when rolling
             * backward.
             */
            goto switch_and_jump;
        }

        /* 2: the test for an instantiated off-page key. */
        if (ikey != NULL) {
            /*
             * If this is the key we originally wanted, we don't care if we're rolling forward or
             * backward, or if it's an overflow key or not, it's what we wanted. Take a copy and
             * wrap up.
             *
             * The key doesn't need to be instantiated, just return.
             */
            if (slot_offset == 0) {
                keyb->data = key_data;
                keyb->size = key_size;
                return (0);
            }

            /*
             * If we wanted a different key and this key is an overflow key:
             *
             * If we're rolling backward, this key is useless to us because it doesn't have a valid
             * prefix: keep rolling backward.
             *
             * If we're rolling forward, there's no work to be done because prefixes skip overflow
             * keys: keep rolling forward.
             */
            if (__wt_cell_type(cell) == WT_CELL_KEY_OVFL)
                goto next;

            /*
             * If we wanted a different key and this key is not an overflow key, it has a valid
             * prefix, we can use it.
             *
             * If rolling backward, take a copy of the key and switch directions, we can roll
             * forward from this key.
             *
             * If rolling forward, replace the key we've been building with this key, it's what we
             * would have built anyway.
             *
             * In short: if it's not an overflow key, take a copy and roll forward.
             */
            keyb->data = key_data;
            keyb->size = key_size;
            direction = FORWARD;
            goto next;
        }

        /* Unpack the on-page cell. */
        __wt_cell_unpack_kv(session, page->dsk, cell, unpack);

        /* 3: the test for an on-page reference to an overflow key. */
        if (unpack->type == WT_CELL_KEY_OVFL || unpack->type == WT_CELL_KEY_OVFL_RM) {
            /*
             * If this is the key we wanted from the start, we don't care if it's an overflow key,
             * get a copy and wrap up.
             *
             * We can race with reconciliation deleting overflow keys. Deleted overflow keys must be
             * instantiated before deletion, acquire the overflow lock and check. If the key has
             * been deleted, restart the slot and get the instantiated key, else read the key before
             * releasing the lock.
             */
            if (slot_offset == 0) {
                __wt_readlock(session, &btree->ovfl_lock);
                if (__wt_cell_type_raw(unpack->cell) == WT_CELL_KEY_OVFL_RM) {
                    __wt_readunlock(session, &btree->ovfl_lock);
                    goto overflow_retry;
                }
                ret = __wt_dsk_cell_data_ref_kv(session, WT_PAGE_ROW_LEAF, unpack, keyb);
                __wt_readunlock(session, &btree->ovfl_lock);
                WT_RET(ret);
                break;
            }

            /*
             * If we wanted a different key:
             *
             * If we're rolling backward, this key is useless to us because it doesn't have a valid
             * prefix: keep rolling backward.
             *
             * If we're rolling forward, there's no work to be done because prefixes skip overflow
             * keys: keep rolling forward.
             */
            goto next;
        }

        /*
         * 4: the test for an on-page reference to a key that isn't prefix compressed.
         */
        if (unpack->prefix == 0) {
            /*
             * If this is the key we originally wanted, we don't care if we're rolling forward or
             * backward, it's what we want. Take a copy and wrap up.
             *
             * If we wanted a different key, this key has a valid prefix, we can use it.
             *
             * If rolling backward, take a copy of the key and switch directions, we can roll
             * forward from this key.
             *
             * If rolling forward there's a bug, we should have found this key while rolling
             * backwards and switched directions then.
             *
             * The key doesn't need to be instantiated, just return.
             */
            WT_RET(__wt_dsk_cell_data_ref_kv(session, WT_PAGE_ROW_LEAF, unpack, keyb));
            if (slot_offset == 0)
                return (0);
            goto switch_and_jump;
        }

        key_data = unpack->data;
        key_size = unpack->size;
        key_prefix = unpack->prefix;

prefix_continue:
        /*
         * Proceed with a prefix-compressed key.
         *
         * Prefix compression means we don't yet have a key, but there's a special case: if the key
         * is part of the group of compressed key prefixes we saved when reading the page into
         * memory, we can build a key for this slot. Otherwise we have to keep rolling forward or
         * backward.
         */
        slot = WT_ROW_SLOT(page, rip);
        if (slot > page->prefix_start && slot <= page->prefix_stop) {
            /*
             * Get the root key's information (the row-store key can change underfoot; explicitly
             * take a copy). Ignore the root key's size and prefix information because it must be
             * large enough (else the current key couldn't have been prefix-compressed based on its
             * value), and it can't have a prefix-compression value, it's a root key which is never
             * prefix-compressed.
             */
            copy = WT_ROW_KEY_COPY(&page->pg_row[page->prefix_start]);

            __wt_row_leaf_key_info(page, copy, NULL, NULL, &group_key, &group_size, &group_prefix);
            if (group_key != NULL) {
                WT_RET(__wt_buf_init(session, keyb, key_prefix + key_size));
                memcpy(keyb->mem, group_key, key_prefix);
                memcpy((uint8_t *)keyb->mem + key_prefix, key_data, key_size);
                keyb->size = key_prefix + key_size;
                /*
                 * If this is the key we originally wanted, we don't care if we're rolling forward
                 * or backward, it's what we want.
                 *
                 * The key doesn't need to be instantiated, just return.
                 */
                if (slot_offset == 0)
                    return (0);
                goto switch_and_jump;
            }
        }

        /*
         * 5: an on-page reference to a key that's prefix compressed.
         *
         * If rolling backward, keep looking for something we can use.
         *
         * If rolling forward, build the full key and keep rolling forward.
         */
        if (direction == BACKWARD) {
            /*
             * If there's a set of keys with identical prefixes, we don't want to instantiate each
             * one, the prefixes are all the same.
             *
             * As we roll backward through the page, track the last time the prefix decreased in
             * size, so we can start with that key during our roll-forward. For a page populated
             * with a single key prefix, we'll be able to instantiate the key we want as soon as we
             * find a key without a prefix.
             */
            if (slot_offset == 0)
                last_prefix = key_prefix;
            if (slot_offset == 0 || last_prefix > key_prefix) {
                jump_rip = rip;
                jump_slot_offset = slot_offset;
                last_prefix = key_prefix;
            }
        }
        if (direction == FORWARD) {
            /*
             * Grow the buffer as necessary as well as ensure data has been copied into local buffer
             * space, then append the suffix to the prefix already in the buffer.
             *
             * Don't grow the buffer unnecessarily or copy data we don't need, truncate the item's
             * CURRENT data length to the prefix bytes before growing the buffer.
             */
            WT_ASSERT(session, keyb->size >= key_prefix);
            keyb->size = key_prefix;
            WT_RET(__wt_buf_grow(session, keyb, key_prefix + key_size));
            memcpy((uint8_t *)keyb->data + key_prefix, key_data, key_size);
            keyb->size = key_prefix + key_size;

            if (slot_offset == 0)
                break;
        }

next:
        switch (direction) {
        case BACKWARD:
            --rip;
            ++slot_offset;
            break;
        case FORWARD:
            ++rip;
            --slot_offset;
            break;
        }
    }

    /*
     * Optionally instantiate the key: there's a cost to figuring out a key value in a leaf page
     * with prefix-compressed keys, amortize the cost by instantiating a copy of the calculated key
     * in allocated memory. We don't instantiate keys when pages are first brought into memory
     * because it's wasted effort if the page is only read by a cursor in sorted order. If, instead,
     * the page is read by a cursor in reverse order, we immediately instantiate periodic keys for
     * the page (otherwise the reverse walk would be insanely slow). If, instead, the page is
     * randomly searched, we instantiate keys as they are accessed (meaning, for example, as long as
     * the binary search only touches one-half of the page, the only keys we instantiate will be in
     * that half of the page).
     */
    if (instantiate) {
        copy = WT_ROW_KEY_COPY(rip_arg);
        __wt_row_leaf_key_info(page, copy, &ikey, &cell, NULL, NULL, NULL);

        /* Check if we raced with another thread instantiating the key before doing real work. */
        if (ikey != NULL)
            return (0);
        WT_RET(__wt_row_ikey_alloc(
          session, WT_PAGE_DISK_OFFSET(page, cell), keyb->data, keyb->size, &ikey));

        /*
         * Serialize the swap of the key into place: on success, update the page's memory footprint,
         * on failure, free the allocated memory.
         */
        if (__wt_atomic_cas_ptr((void *)&WT_ROW_KEY_COPY(rip), copy, ikey))
            __wt_cache_page_inmem_incr(session, page, sizeof(WT_IKEY) + ikey->size);
        else
            __wt_free(session, ikey);
    }
    return (0);
}

/*
 * __wt_row_ikey_alloc --
 *     Instantiate a key in a WT_IKEY structure.
 */
int
__wt_row_ikey_alloc(
  WT_SESSION_IMPL *session, uint32_t cell_offset, const void *key, size_t size, WT_IKEY **ikeyp)
{
    WT_IKEY *ikey;

    WT_ASSERT(session, key != NULL); /* quiet clang scan-build */

    /*
     * Allocate memory for the WT_IKEY structure and the key, then copy the key into place.
     */
    WT_RET(__wt_calloc(session, 1, sizeof(WT_IKEY) + size, &ikey));
    ikey->size = WT_STORE_SIZE(size);
    ikey->cell_offset = cell_offset;
    memcpy(WT_IKEY_DATA(ikey), key, size);
    *ikeyp = ikey;
    return (0);
}

/*
 * __wt_row_ikey_incr --
 *     Instantiate a key in a WT_IKEY structure and increment the page's memory footprint.
 */
int
__wt_row_ikey_incr(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t cell_offset, const void *key,
  size_t size, WT_REF *ref)
{
    WT_RET(__wt_row_ikey(session, cell_offset, key, size, ref));

    __wt_cache_page_inmem_incr(session, page, sizeof(WT_IKEY) + size);

    return (0);
}

/*
 * __wt_row_ikey --
 *     Instantiate a key in a WT_IKEY structure.
 */
int
__wt_row_ikey(
  WT_SESSION_IMPL *session, uint32_t cell_offset, const void *key, size_t size, WT_REF *ref)
{
    WT_IKEY *ikey;

    WT_RET(__wt_row_ikey_alloc(session, cell_offset, key, size, &ikey));

#ifdef HAVE_DIAGNOSTIC
    {
        uintptr_t oldv;

        oldv = (uintptr_t)ref->ref_ikey;
        WT_DIAGNOSTIC_YIELD;

        /*
         * We should never overwrite an instantiated key, and we should never instantiate a key
         * after a split.
         */
        WT_ASSERT(session, oldv == 0 || (oldv & WT_IK_FLAG) != 0);
        WT_ASSERT(session, ref->state != WT_REF_SPLIT);
        WT_ASSERT(session, __wt_atomic_cas_ptr(&ref->ref_ikey, (WT_IKEY *)oldv, ikey));
    }
#else
    ref->ref_ikey = ikey;
#endif
    return (0);
}
