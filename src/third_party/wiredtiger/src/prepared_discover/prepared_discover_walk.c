/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __prepared_discover_btree_has_prepare --
 *     Check the metadata entry for a btree to see whether it included prepared updates.
 */
static int
__prepared_discover_btree_has_prepare(WT_SESSION_IMPL *session, const char *config, bool *has_prepp)
{
    WT_CONFIG ckptconf;
    WT_CONFIG_ITEM cval, key, value;
    WT_DECL_RET;

    *has_prepp = false;

    /* This configuration parsing is copied out of the rollback to stable implementation */
    WT_RET(__wt_config_getones(session, config, "checkpoint", &cval));
    __wt_config_subinit(session, &ckptconf, &cval);
    for (; __wt_config_next(&ckptconf, &key, &cval) == 0;) {
        ret = __wt_config_subgets(session, &cval, "prepare", &value);
        if (ret == 0) {
            if (value.val)
                *has_prepp = true;
        }
        WT_RET_NOTFOUND_OK(ret);
    }
    return (0);
}

/*
 * __prepared_discover_process_ondisk_kv --
 *     Found an on-disk prepared value, process it into a transaction structure.
 */
static int
__prepared_discover_process_ondisk_kv(WT_SESSION_IMPL *session, WT_REF *ref, WT_ROW *rip,
  uint64_t recno, WT_ITEM *row_key, WT_CELL_UNPACK_KV *vpack)
{
    WT_DECL_RET;
    WT_ITEM *key;
    WT_PAGE *page;
    WT_TIME_WINDOW *tw;
    uint8_t *memp;

    page = ref->page;
    if (rip != NULL) {
        if (row_key != NULL)
            key = row_key;
        else {
            /* Unpack a row key. */
            WT_ERR(__wt_scr_alloc(session, 0, &key));
            WT_ERR(__wt_row_leaf_key(session, page, rip, key, false));
        }
    } else {
        /* Manufacture a column key. */
        WT_ERR(__wt_scr_alloc(session, WT_INTPACK64_MAXSIZE, &key));
        memp = key->mem;
        WT_ERR(__wt_vpack_uint(&memp, 0, recno));
        key->size = WT_PTRDIFF(memp, key->data);
    }

    /* Retrieve the time window from the unpacked value cell. */
    __wt_cell_get_tw(vpack, &tw);

    /* Add an entry for this key to the transaction structure */
    if (rip != NULL)
        WT_ERR(
          __wti_prepared_discover_add_artifact_ondisk_row(session, tw->start_prepared_id, tw, key));
    else
        WT_ASSERT_ALWAYS(
          session, false, "Column store prepared transaction discovery not supported");

err:
    if (rip == NULL || row_key == NULL)
        __wt_scr_free(session, &key);
    return (ret);
}

/*
 * __prepared_discover_check_ondisk_kv --
 *     Check the on-disk K/V to see if it was prepared.
 */
static int
__prepared_discover_check_ondisk_kv(WT_SESSION_IMPL *session, WT_REF *ref, WT_ROW *rip,
  uint64_t recno, WT_ITEM *row_key, WT_CELL_UNPACK_KV *vpack)
{
    WT_TIME_WINDOW *tw;

    /*
     * Prepared artifacts in the history store are processed when earlier records from the
     * transaction are also found in the data store.
     */
    if (WT_IS_HS(session->dhandle))
        return (0);

    /* Retrieve the time window from the unpacked value cell. */
    __wt_cell_get_tw(vpack, &tw);

    /* Done if the record wasn't prepared. */
    if (!WT_TIME_WINDOW_HAS_PREPARE(tw))
        return (0);

    WT_RET(__prepared_discover_process_ondisk_kv(session, ref, rip, recno, row_key, vpack));

    return (0);
}

/*
 * __prepared_discover_process_prepared_update --
 *     We found a prepared update, add it into a prepared transaction context.
 */
static int
__prepared_discover_process_prepared_update(WT_SESSION_IMPL *session, WT_ITEM *key, WT_UPDATE *upd)
{
    uint64_t prepared_id;

    WT_ASSERT(
      session, upd->prepare_state != WT_PREPARE_INIT && upd->prepare_state != WT_PREPARE_RESOLVED);

    prepared_id = upd->prepared_id;
    WT_RET(__wti_prepared_discover_add_artifact_upd(session, prepared_id, key, upd));
    return (0);
}

/*
 * __prepared_discover_process_update_list --
 *     Review an update list looking for prepared updates. If a prepared update is found, insert it
 *     into a pending prepared transaction structure.
 */
static int
__prepared_discover_process_update_list(
  WT_SESSION_IMPL *session, WT_ITEM *key, WT_UPDATE *first_upd)
{
    WT_UPDATE *upd;

    for (upd = first_upd; upd != NULL; upd = upd->next) {
        /*
         * Prepared updates must be at the start of the chain, so the first time an update that
         * hasn't been prepared is seen, it's safe to terminate the update chain traversal
         */
        if (upd->prepare_state == WT_PREPARE_INIT || upd->prepare_state == WT_PREPARE_RESOLVED) {
            break;
        }
        WT_RET(__prepared_discover_process_prepared_update(session, key, upd));
    }

    return (0);
}

/*
 * __prepared_discover_process_insert_list --
 *     Review an insert list looking for prepared entries that need to be added to had stable
 *     updates.
 */
static int
__prepared_discover_process_insert_list(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_INSERT_HEAD *head)
{
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(key_string);
    WT_DECL_RET;
    WT_INSERT *ins;
    uint64_t recno;
    uint8_t *memp;

    WT_ERR(
      __wt_scr_alloc(session, page->type == WT_PAGE_ROW_LEAF ? 0 : WT_INTPACK64_MAXSIZE, &key));

    WT_ERR(__wt_scr_alloc(session, 0, &key_string));
    WT_SKIP_FOREACH (ins, head)
        if (ins->upd != NULL) {
            if (page->type == WT_PAGE_ROW_LEAF) {
                key->data = WT_INSERT_KEY(ins);
                key->size = WT_INSERT_KEY_SIZE(ins);
            } else {
                recno = WT_INSERT_RECNO(ins);
                memp = key->mem;
                WT_ERR(__wt_vpack_uint(&memp, 0, recno));
                key->size = WT_PTRDIFF(memp, key->data);
            }

            WT_ERR(__prepared_discover_process_update_list(session, key, ins->upd));
        }

err:
    __wt_scr_free(session, &key);
    __wt_scr_free(session, &key_string);
    return (ret);
}

/*
 * __prepared_discover_process_row_store_leaf_page --
 *     Find and process any prepared artifacts on this row store leaf page. It could either be clean
 *     or have been modified. So handle the full possible page structure, not just a clean image.
 */
static int
__prepared_discover_process_row_store_leaf_page(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_CELL_UNPACK_KV *vpack, _vpack;
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_INSERT_HEAD *insert;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_UPDATE *upd;
    uint32_t i;

    page = ref->page;

    WT_RET(__wt_scr_alloc(session, 0, &key));

    /*
     * Review the insert list for keys before the first entry on the disk page.
     */
    if ((insert = WT_ROW_INSERT_SMALLEST(page)) != NULL)
        WT_ERR(__prepared_discover_process_insert_list(session, page, insert));

    /*
     * Review updates that belong to keys that are on the disk image, as well as for keys inserted
     * since the page was read from disk.
     */
    WT_ROW_FOREACH (page, rip, i) {

        /* Process either the update list or disk image cell for the next entry on the page */
        if ((upd = WT_ROW_UPDATE(page, rip)) != NULL) {
            WT_ERR(__wt_row_leaf_key(session, page, rip, key, false));
            WT_ERR(__prepared_discover_process_update_list(session, key, upd));
        } else {
            /* If there was no update list, check the disk image cell for a prepared update */
            vpack = &_vpack;
            __wt_row_leaf_value_cell(session, page, rip, vpack);

            WT_ERR(__prepared_discover_check_ondisk_kv(session, ref, rip, 0, NULL, vpack));
        }

        /* Walk through any intermediate insert list. */
        if ((insert = WT_ROW_INSERT(page, rip)) != NULL) {
            WT_ERR(__prepared_discover_process_insert_list(session, page, insert));
        }
    }

err:
    __wt_scr_free(session, &key);
    return (ret);
}

/*
 * __prepared_discover_process_leaf_page --
 *     Review the content of a leaf page discovering and processing prepared updates.
 */
static int
__prepared_discover_process_leaf_page(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_PAGE *page;

    page = ref->page;

    switch (page->type) {
    case WT_PAGE_ROW_LEAF:
        WT_RET(__prepared_discover_process_row_store_leaf_page(session, ref));
        break;
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_VAR:
        WT_ASSERT_ALWAYS(session, false, "Prepared discovery does not support column stores");
        /* Fall through. */
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        /* This function is not called for internal pages. */
        WT_ASSERT(session, false);
        /* Fall through. */
    default:
        WT_RET(__wt_illegal_value(session, page->type));
    }

    return (0);
}

/*
 * __prepared_discover_tree_walk_skip --
 *     Skip if rollback to stable doesn't require reading this page.
 */
static int
__prepared_discover_tree_walk_skip(
  WT_SESSION_IMPL *session, WT_REF *ref, void *context, bool visible_all, bool *skipp)
{
    WT_ADDR *addr;
    WT_CELL_UNPACK_ADDR vpack;
    WT_PAGE_DELETED *page_del;
    WT_TIME_AGGREGATE *ta;

    WT_UNUSED(context);
    WT_UNUSED(visible_all);
    addr = ref->addr;

    *skipp = false; /* Default to reading */

    /*
     * Fast deleted pages can be ignored here, unless they were generated by a prepared transaction.
     * This code doesn't currently support truncate within a prepared transaction - trigger a fatal
     * error if we encounter that.
     */
    if (WT_REF_GET_STATE(ref) == WT_REF_DELETED &&
      WT_REF_CAS_STATE(session, ref, WT_REF_DELETED, WT_REF_LOCKED)) {
        page_del = ref->page_del;
        WT_ASSERT_ALWAYS(session, page_del->prepare_state == WT_PREPARE_INIT,
          "Prepared transaction discovery does not support truncate operations");
    }
    /*
     * Any other deleted page can't have prepared content that needs to be discovered, so it is safe
     * to skip it.
     *
     * TODO: handle prepared fast delete.
     */
    if (WT_REF_GET_STATE(ref) == WT_REF_DELETED) {
        *skipp = true;
        return (0);
    }

    /*
     * Otherwise, if the page state is other than on disk, it is probably in-memory and we can't
     * really on the address or cell information from the disk image to decide if it has prepared
     * content to discover.
     */
    if (WT_REF_GET_STATE(ref) != WT_REF_DISK)
        return (0);

    /*
     * Check whether this on-disk page or it's children has any prepared content.
     */
    if (!__wt_off_page(ref->home, addr)) {
        /* Check if the page is obsolete using the page disk address. */
        __wt_cell_unpack_addr(session, ref->home->dsk, (WT_CELL *)addr, &vpack);
        /* Retrieve the time aggregate from the unpacked address cell. */
        __wt_cell_get_ta(&vpack, &ta);
        if (!ta->prepare)
            *skipp = true;
    } else if (addr != NULL) {
        if (!addr->ta.prepare)
            *skipp = true;
    } else
        WT_ASSERT_ALWAYS(
          session, false, "Prepared discovery walk encountered a page without a valid address");

    return (0);
}

/*
 * __prepared_discover_walk_one_tree --
 *     Walk a btree handle that is known to have prepared artifacts, attaching them to transaction
 *     handles as they are discovered.
 */
static int
__prepared_discover_walk_one_tree(WT_SESSION_IMPL *session, const char *uri)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_REF *ref;
    uint32_t flags;

    /* Open a handle for processing. */
    ret = __wt_session_get_dhandle(session, uri, NULL, NULL, 0);
    if (ret != 0)
        WT_RET_MSG(session, ret, "%s: unable to open handle%s", uri,
          ret == EBUSY ? ", error indicates handle is unavailable due to concurrent use" : "");

    btree = S2BT(session);
    /* There is nothing to do on an empty tree. */
    if (btree->root.page != NULL) {
        flags = WT_READ_NO_EVICT | WT_READ_VISIBLE_ALL | WT_READ_WONT_NEED | WT_READ_SEE_DELETED;
        ref = NULL;
        while ((ret = __wt_tree_walk_custom_skip(
                  session, &ref, __prepared_discover_tree_walk_skip, NULL, flags)) == 0 &&
          ref != NULL) {

            if (F_ISSET(ref, WT_REF_FLAG_LEAF))
                WT_ERR(__prepared_discover_process_leaf_page(session, ref));
        }
    }
err:
    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __wt_prepared_discover_filter_apply_handles --
 *     Review the metadata and identify btrees that have prepared content that needs to be
 *     discovered
 */
int
__wt_prepared_discover_filter_apply_handles(WT_SESSION_IMPL *session)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *uri, *config;
    bool has_prepare;

    /*
     * TODO: how careful does this need to be about concurrent schema operations? If this step needs
     * to be exclusive in some way it should probably accumulate a set of relevant handles before
     * releasing that access and doing the processing after generating the list.
     */
    WT_RET(__wt_metadata_cursor(session, &cursor));

    while ((ret = cursor->next(cursor)) == 0) {
        WT_ERR(cursor->get_key(cursor, &uri));
        /* Only interested in btree handles that aren't the metadata */
        if (!WT_BTREE_PREFIX(uri) || strcmp(uri, WT_METAFILE_URI) == 0)
            continue;

        WT_ERR_NOTFOUND_OK(cursor->get_value(cursor, &config), true);
        if (ret == WT_NOTFOUND)
            config = NULL;
        /* Check to see if there is any prepared content in the handle */
        WT_ERR(__prepared_discover_btree_has_prepare(session, config, &has_prepare));
        if (!has_prepare)
            continue;
        WT_ERR(__prepared_discover_walk_one_tree(session, uri));
    }
    if (ret == WT_NOTFOUND)
        ret = 0;
err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));

    return (ret);
}
