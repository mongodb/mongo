/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __cursor_prepared_discover_list_create(
  WT_SESSION_IMPL *, WT_CURSOR_PREPARE_DISCOVERED *);
static int __cursor_prepared_discover_setup(WT_SESSION_IMPL *, WT_CURSOR_PREPARE_DISCOVERED *);

/*
 * __cursor_prepared_discover_next --
 *     WT_CURSOR->next method for the prepared transaction cursor type.
 */
static int
__cursor_prepared_discover_next(WT_CURSOR *cursor)
{
    WT_CURSOR_PREPARE_DISCOVERED *cursor_prepare;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    size_t size;

    cursor_prepare = (WT_CURSOR_PREPARE_DISCOVERED *)cursor;
    CURSOR_API_CALL(cursor, session, ret, next, NULL);

    if (cursor_prepare->list == NULL || cursor_prepare->list[cursor_prepare->next] == 0) {
        F_CLR(cursor, WT_CURSTD_KEY_SET);
        WT_ERR(WT_NOTFOUND);
    }

    WT_ERR(__wt_struct_size(
      session, &size, cursor->key_format, cursor_prepare->list[cursor_prepare->next]));
    WT_ERR(__wt_buf_initsize(session, &cursor->key, size));
    WT_ERR(__wt_struct_pack(session, cursor->key.mem, size, cursor->key_format,
      cursor_prepare->list[cursor_prepare->next]));
    ++cursor_prepare->next;

    F_SET(cursor, WT_CURSTD_KEY_INT);

err:
    API_END_RET(session, ret);
}

/*
 * __cursor_prepared_discover_reset --
 *     WT_CURSOR->reset method for the prepared transaction cursor type.
 */
static int
__cursor_prepared_discover_reset(WT_CURSOR *cursor)
{
    WT_CURSOR_PREPARE_DISCOVERED *cursor_prepare;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cursor_prepare = (WT_CURSOR_PREPARE_DISCOVERED *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, NULL);

    cursor_prepare->next = 0;
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

err:
    API_END_RET(session, ret);
}

/*
 * __cursor_prepared_discover_close --
 *     WT_CURSOR->close method for the prepared transaction cursor type.
 */
static int
__cursor_prepared_discover_close(WT_CURSOR *cursor)
{
    WT_CURSOR_PREPARE_DISCOVERED *cursor_prepare;
    WT_DECL_RET;
    WT_PENDING_PREPARED_ITEM *item;
    WT_PENDING_PREPARED_MAP *pending_prepare_items;
    WT_SESSION_IMPL *session;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_OP *op;
    uint64_t i, j;
    uint64_t unclaimed_count;
    cursor_prepare = (WT_CURSOR_PREPARE_DISCOVERED *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, NULL);

    txn_global = &S2C(session)->txn_global;
    pending_prepare_items = &txn_global->pending_prepare_items;
    unclaimed_count = 0;
    if (pending_prepare_items->hash != NULL) {
        for (i = 0; i < pending_prepare_items->hash_size; i++) {
            while ((item = TAILQ_FIRST(&pending_prepare_items->hash[i])) != NULL) {
                /*
                 * Claimed prepare transactions should have been removed from the hash map already.
                 * Increase the counter if we find unclaimed item left in map.
                 */
                unclaimed_count++;
                TAILQ_REMOVE(&pending_prepare_items->hash[i], item, hashq);
                /* Clean up memory of unclaimed mod array */
                for (j = 0, op = item->mod; j < item->mod_count; j++, op++) {
                    __wt_txn_op_free(session, op);
                }
                __wt_free(session, item->mod);
                __wt_free(session, item);
            }
        }
        __wt_free(session, pending_prepare_items->hash);
        memset((void *)pending_prepare_items, 0, sizeof(WT_PENDING_PREPARED_MAP));
    }
    if (unclaimed_count > 0)
        WT_ERR_MSG(
          session, WT_ERROR, "Found %" PRIu64 " unclaimed prepared transactions", unclaimed_count);
err:

    __wt_free(session, cursor_prepare->list);
    __wt_cursor_close(cursor);
    API_END_RET(session, ret);
}

/*
 * __wt_cursor_prepared_discover_open --
 *     WT_SESSION->open_cursor method for the prepared transaction cursor type.
 */
int
__wt_cursor_prepared_discover_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *other,
  const char *cfg[], WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wti_cursor_get_value_notsup,                  /* get-value */
      __wti_cursor_get_raw_key_value_notsup,          /* get-raw-key-value */
      __wti_cursor_set_key_notsup,                    /* set-key */
      __wti_cursor_set_value_notsup,                  /* set-value */
      __wti_cursor_compare_notsup,                    /* compare */
      __wti_cursor_equals_notsup,                     /* equals */
      __cursor_prepared_discover_next,                /* next */
      __wt_cursor_notsup,                             /* prev */
      __cursor_prepared_discover_reset,               /* reset */
      __wt_cursor_notsup,                             /* search */
      __wt_cursor_search_near_notsup,                 /* search-near */
      __wt_cursor_notsup,                             /* insert */
      __wt_cursor_modify_notsup,                      /* modify */
      __wt_cursor_notsup,                             /* update */
      __wt_cursor_notsup,                             /* remove */
      __wt_cursor_notsup,                             /* reserve */
      __wt_cursor_config_notsup,                      /* reconfigure */
      __wt_cursor_notsup,                             /* largest_key */
      __wt_cursor_config_notsup,                      /* bound */
      __wt_cursor_notsup,                             /* cache */
      __wt_cursor_reopen_notsup,                      /* reopen */
      __wt_cursor_checkpoint_id,                      /* checkpoint ID */
      __cursor_prepared_discover_close);              /* close */
    WT_CURSOR *cursor;
    WT_CURSOR_PREPARE_DISCOVERED *cursor_prepare;
    WT_DECL_RET;

    WT_VERIFY_OPAQUE_POINTER(WT_CURSOR_PREPARE_DISCOVERED);

    WT_UNUSED(other);
    WT_UNUSED(cfg);

    /*
     * TODO: This should acquire a prepared transaction discovery lock read/write lock in write
     * mode. Any thread wanting to commit a prepared transaction should acquire that lock in read
     * mode (or return an error). If the write lock is already held, this should exit immediately.
     */
    WT_RET(__wt_calloc_one(session, &cursor_prepare));
    cursor = (WT_CURSOR *)cursor_prepare;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->key_format = "Q";  /* The key is an unsigned 64 bit number. */
    cursor->value_format = ""; /* Empty for now, will probably have something eventually */

    /*
     * Start the prepared transaction cursor which will fill in the cursor's list. Acquire the
     * schema lock, we need a consistent view of the metadata when scanning for prepared artifacts.
     */
    WT_WITH_CHECKPOINT_LOCK(session,
      WT_WITH_SCHEMA_LOCK(
        session, ret = __cursor_prepared_discover_setup(session, cursor_prepare)));
    WT_ERR(ret);
    WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

    if (0) {
err:
        WT_TRET(__cursor_prepared_discover_close(cursor));
        *cursorp = NULL;
    }

    return (ret);
}

/*
 * __cursor_prepared_discover_setup --
 *     Setup a prepared transaction cursor on open. This will populate the data structures for the
 *     cursor to traverse. Some data structures live in this cursor, others live in the connection
 *     handle, since they can be claimed by other sessions while the cursor is open.
 */
static int
__cursor_prepared_discover_setup(
  WT_SESSION_IMPL *session, WT_CURSOR_PREPARE_DISCOVERED *cursor_prepare)
{
    WT_RET(__wt_prepared_discover_filter_apply_handles(session));
    WT_RET(__cursor_prepared_discover_list_create(session, cursor_prepare));
    return (0);
}

/*
 * __cursor_prepared_discover_list_create --
 *     Review the pending prepared transactions in the transaction global list and create a list of
 *     transactions for this cursor to traverse through. The cursor could just use that list
 *     directly, but the level of indirection feels as though it will be helpful.
 */
static int
__cursor_prepared_discover_list_create(
  WT_SESSION_IMPL *session, WT_CURSOR_PREPARE_DISCOVERED *cursor_prepare)
{
    WT_PENDING_PREPARED_ITEM *item;
    WT_PENDING_PREPARED_MAP *pending_prepare_items;
    WT_TXN_GLOBAL *txn_global;

    txn_global = &S2C(session)->txn_global;
    pending_prepare_items = &txn_global->pending_prepare_items;

    /* If no transactions were discovered, there is nothing more to do here */
    if (pending_prepare_items->hash == NULL)
        return (0);

    /* Iterate through the hash map and populate the list as we go */
    for (uint64_t i = 0; i < pending_prepare_items->hash_size; i++) {
        TAILQ_FOREACH (item, &pending_prepare_items->hash[i], hashq) {
            /* Grow the list to accommodate this new item plus null terminator */
            WT_RET(__wt_realloc_def(session, &cursor_prepare->list_allocated,
              cursor_prepare->list_next + 2, &cursor_prepare->list));

            /* Add the prepared transaction ID to the list */
            cursor_prepare->list[cursor_prepare->list_next] = item->prepared_id;
            cursor_prepare->list_next++;
        }
    }

    /* Null-terminate the list */
    if (cursor_prepare->list_next > 0) {
        cursor_prepare->list[cursor_prepare->list_next] = 0;
    }

    return (0);
}
