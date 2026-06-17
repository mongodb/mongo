/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __layered_create_missing_ingest_table --
 *     Create a missing ingest table from an existing layered table configuration.
 */
static int
__layered_create_missing_ingest_table(
  WT_SESSION_IMPL *session, const char *uri, const char *layered_cfg)
{
    WT_CONFIG_ITEM key_format, value_format;
    WT_DECL_ITEM(ingest_config);
    WT_DECL_RET;

    WT_ERR(__wt_config_getones(session, layered_cfg, "key_format", &key_format));
    WT_ERR(__wt_config_getones(session, layered_cfg, "value_format", &value_format));

    /* FIXME-WT-14728: Refactor this with __create_layered? */
    WT_ERR(__wt_scr_alloc(session, 0, &ingest_config));
    WT_ERR(__wt_buf_fmt(session, ingest_config,
      "key_format=\"%.*s\",value_format=\"%.*s\","
      "in_memory=true,log=(enabled=false),"
      "disaggregated=(page_log=none,storage_source=none)",
      (int)key_format.len, key_format.str, (int)value_format.len, value_format.str));

    WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_create(session, uri, ingest_config->data));

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Created missing ingest table \"%s\" from \"%s\"", uri, layered_cfg);

err:
    __wt_scr_free(session, &ingest_config);
    return (ret);
}

/*
 * __disagg_discard_old_checkpoint_check --
 *     Compare the checkpoint name in the old and new metadata config strings. Check if they are the
 *     same checkpoint. If the checkpoint has advanced, the old one can be discarded.
 */
static int
__disagg_discard_old_checkpoint_check(WT_SESSION_IMPL *session, const char *cfg_current,
  const char *cfg_new, const char **checkpoint_name, bool *discardp)
{
    WT_DECL_RET;
    uint64_t checkpoint_time, checkpoint_time_new;
    int64_t checkpoint_order, checkpoint_order_new;
    const char *checkpoint_name_new;

    checkpoint_order = checkpoint_order_new = 0;
    checkpoint_time = checkpoint_time_new = 0;
    *checkpoint_name = checkpoint_name_new = NULL;

    WT_ERR_NOTFOUND_OK(__wt_ckpt_last_name(session, cfg_current, checkpoint_name, &checkpoint_order,
                         &checkpoint_time),
      true);
    /* Early exit if we can't find the configuration of last checkpoint. */
    if (ret == WT_NOTFOUND) {
        WT_ASSERT(session, *checkpoint_name == NULL);
        *discardp = false;
        return (0);
    }

    /*
     * It is possible that the new checkpoint is empty (e.g. all disagg tables were dropped). The
     * state has still advanced, so discard the old checkpoint.
     */
    WT_ERR_NOTFOUND_OK(__wt_ckpt_last_name(session, cfg_new, &checkpoint_name_new,
                         &checkpoint_order_new, &checkpoint_time_new),
      true);
    if (ret == WT_NOTFOUND) {
        WT_ASSERT(session, checkpoint_name_new == NULL);
        *discardp = false;
        return (0);
    }

    /*
     * Treat the checkpoint order and time configurations as the source of truth when determining
     * whether the checkpoint has changed.
     */
    *discardp =
      !(checkpoint_order == checkpoint_order_new && checkpoint_time == checkpoint_time_new);

#ifdef HAVE_DIAGNOSTIC
    if (!*discardp)
        WT_ASSERT(session, strcmp(*checkpoint_name, checkpoint_name_new) == 0);
#endif
err:
    __wt_free(session, checkpoint_name_new);
    return (ret);
}

/*
 * __disagg_save_checkpoint_meta_local --
 *     Update the local metadata entry with the supplied checkpoint configuration.
 */
static int
__disagg_save_checkpoint_meta_local(WT_SESSION_IMPL *session, const WT_DISAGG_METADATA *metadata)
{
    WT_CURSOR *md_cursor;
    WT_DECL_ITEM(metadata_cfg);
    WT_DECL_ITEM(old_uri_buf);
    WT_DECL_RET;
    char *cfg_current_copy, *cfg_new;
    const char *cfg[3], *checkpoint_name, *cfg_current, *metadata_key;
    bool discard;

    cfg_current_copy = cfg_new = NULL;
    checkpoint_name = NULL;
    discard = false;
    md_cursor = NULL;
    metadata_key = WT_DISAGG_METADATA_URI;

    /* Open up a metadata cursor pointing at our table */
    WT_ERR(__wt_metadata_cursor(session, &md_cursor));

    /* Pull the value out. */
    md_cursor->set_key(md_cursor, metadata_key);
    WT_ERR(md_cursor->search(md_cursor));
    WT_ERR(md_cursor->get_value(md_cursor, &cfg_current));
    /* Copy the value since we don't own the memory after calling get_value(). */
    WT_ERR(__wt_strdup(session, cfg_current, &cfg_current_copy));

    /* Create the new checkpoint config string. */
    WT_ERR(__wt_scr_alloc(session, 0, &metadata_cfg));
    WT_ERR(__wt_buf_fmt(session, metadata_cfg, "checkpoint=%.*s", (int)metadata->checkpoint_len,
      metadata->checkpoint));

    cfg[0] = cfg_current_copy;
    cfg[1] = metadata_cfg->data;
    cfg[2] = NULL;
    WT_ERR(__wt_config_collapse(session, cfg, &cfg_new));

    /* Put in our new config. */
    WT_ERR(__wt_metadata_insert(session, metadata_key, cfg_new));

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Updated the local metadata for key \"%s\" to include the new checkpoint: \"%.*s\"",
      metadata_key, (int)metadata->checkpoint_len, metadata->checkpoint);

    /* Throw away any references to the old disaggregated metadata table checkpoint. */
    WT_ERR(__disagg_discard_old_checkpoint_check(
      session, cfg_current_copy, cfg_new, &checkpoint_name, &discard));
    if (discard) {
        WT_ERR(__wt_scr_alloc(session, 0, &old_uri_buf));
        WT_ERR(__wt_buf_fmt(session, old_uri_buf, "%s/%s", metadata_key, checkpoint_name));
        WT_WITHOUT_DHANDLE(session, ret = __wti_conn_dhandle_outdated(session, old_uri_buf->data));
        WT_ERR_MSG_CHK(session, ret, "Marking data handles outdated failed: \"%s\"",
          (const char *)old_uri_buf->data);
    }
err:
    __wt_free(session, cfg_current_copy);
    __wt_free(session, cfg_new);
    __wt_free(session, checkpoint_name);
    __wt_scr_free(session, &metadata_cfg);
    __wt_scr_free(session, &old_uri_buf);

    if (md_cursor != NULL)
        WT_TRET(__wt_metadata_cursor_release(session, &md_cursor));
    return (ret);
}

/*
 * __disagg_bound_cursor --
 *     Bound the cursor to the given URI prefix.
 */
static int
__disagg_bound_cursor(WT_SESSION_IMPL *session, WT_CURSOR *cursor, const char *uri_prefix)
{
    WT_DECL_ITEM(upper_bound_buf);
    WT_DECL_RET;
    size_t len;

    len = strlen(uri_prefix);
    WT_ASSERT(session, len > 0 && uri_prefix[len - 1] == ':');

    cursor->set_key(cursor, uri_prefix);
    WT_ERR(cursor->bound(cursor, "bound=lower"));

    /*
     * The prefix must end with ':'; the upper bound is derived by replacing that ':' with ';', the
     * next ASCII character, so the scan covers exactly the entries for that URI scheme.
     */
    WT_ERR(__wt_scr_alloc(session, len + 1, &upper_bound_buf));
    WT_ERR(__wt_buf_set(session, upper_bound_buf, uri_prefix, len));
    ((char *)upper_bound_buf->data)[len - 1] = ':' + 1; /* Get the upper bound. */
    ((char *)upper_bound_buf->data)[len] = '\0';
    cursor->set_key(cursor, (const char *)upper_bound_buf->data);
    WT_ERR(cursor->bound(cursor, "bound=upper"));

err:
    __wt_scr_free(session, &upper_bound_buf);
    return (ret);
}

/* Indexes for each cursor type (plus count). */
enum WT_DISAGG_CKPT_PICKUP_CURSORS {
    WT_DISAGG_CURSOR_COLGROUP = 0,
    WT_DISAGG_CURSOR_FILE,
    WT_DISAGG_CURSOR_LAYERED,
    WT_DISAGG_CURSOR_TABLE,
    WT_DISAGG_CURSOR_COUNT /* Must be last. */
};

/* Prefixes for each cursor type. */
static const char *const __disagg_cursor_prefixes[WT_DISAGG_CURSOR_COUNT] = {
  "colgroup:", "file:", "layered:", "table:"};

/* Length of each prefix string. */
static const size_t __disagg_cursor_prefix_lengths[WT_DISAGG_CURSOR_COUNT] = {9, 5, 8, 6};

/*
 * __disagg_cursor_next --
 *     Advance a cursor and return its key, or NULL if the cursor is exhausted.
 */
static int
__disagg_cursor_next(WT_CURSOR *cursor, const char **keyp)
{
    WT_DECL_RET;

    WT_RET_NOTFOUND_OK(ret = cursor->next(cursor));
    if (ret == WT_NOTFOUND) {
        *keyp = NULL;
        return (0);
    }
    WT_RET(cursor->get_key(cursor, keyp));

    return (0);
}

/*
 * __disagg_table_name --
 *     Return the table name embedded in a metadata key, stripping the URI prefix and any
 *     scheme-specific suffix. The result is a pointer into the key buffer with a separate length.
 *     It is not null-terminated at that boundary.
 */
static void
__disagg_table_name(const char *key, int idx, const char **namep, size_t *lenp)
{
    size_t len;
    const char *name;

    name = key + __disagg_cursor_prefix_lengths[idx];
    len = strlen(name);

    if (idx == WT_DISAGG_CURSOR_FILE) {
        if (len >= 10 && memcmp(name + len - 10, ".wt_stable", 10) == 0)
            len -= 10;
        else if (len >= 3 && memcmp(name + len - 3, ".wt", 3) == 0)
            len -= 3;
    }

    *namep = name;
    *lenp = len;
}

/*
 * __disagg_file_skip_local --
 *     Advance a file: cursor past any non-shared metadata entries.
 */
static int
__disagg_file_skip_local(WT_CURSOR *cursor, const char **keyp)
{
    size_t key_name_len, name_len;
    const char *name;

    while (*keyp != NULL) {
        WT_ASSERT((WT_SESSION_IMPL *)cursor->session, WT_PREFIX_MATCH(*keyp, "file:"));
        key_name_len = strlen(*keyp + __disagg_cursor_prefix_lengths[WT_DISAGG_CURSOR_FILE]);
        __disagg_table_name(*keyp, WT_DISAGG_CURSOR_FILE, &name, &name_len);
        if (name_len < key_name_len) /* A recognized suffix was stripped. */
            break;
        WT_RET(__disagg_cursor_next(cursor, keyp));
    }
    return (0);
}

/*
 * __disagg_update_min --
 *     If the table name embedded in key is less than the current minimum, replace the minimum. Does
 *     nothing when key is NULL (cursor exhausted).
 */
static void
__disagg_update_min(const char *key, int idx, const char **currentp, size_t *current_lenp)
{
    size_t name_len;
    const char *name;

    if (key == NULL)
        return;
    __disagg_table_name(key, idx, &name, &name_len);
    if (*currentp == NULL || __wt_string_slice_cmp(name, name_len, *currentp, *current_lenp) < 0) {
        *currentp = name;
        *current_lenp = name_len;
    }
}

/*
 * __disagg_key_at_table --
 *     Return true if the table name embedded in key matches the given (current, current_len) name.
 *     Returns false when key is NULL (cursor exhausted).
 */
static bool
__disagg_key_at_table(const char *key, int idx, const char *current, size_t current_len)
{
    size_t name_len;
    const char *name;

    if (key == NULL)
        return (false);
    __disagg_table_name(key, idx, &name, &name_len);
    return (__wt_string_slice_cmp(name, name_len, current, current_len) == 0);
}

/*
 * __disagg_insert_meta --
 *     Copy the current entry from a shared metadata cursor into the local metadata table.
 */
static int
__disagg_insert_meta(WT_SESSION_IMPL *session, WT_CURSOR *sh_cursor, WT_CURSOR *md_cursor)
{
    WT_DECL_RET;
    const char *key, *value;

    WT_ERR(sh_cursor->get_key(sh_cursor, &key));
    WT_ERR(sh_cursor->get_value(sh_cursor, &value));
    md_cursor->set_key(md_cursor, key);
    md_cursor->set_value(md_cursor, value);
    WT_ERR_MSG_CHK(
      session, md_cursor->insert(md_cursor), "Failed to insert metadata for key \"%s\"", key);
    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Inserted new key to the local metadata \"%s\": \"%s\"", key, value);
    WT_STAT_CONN_INCR(session, disagg_pick_up_file_meta_inserted);

err:
    return (ret);
}

/*
 * __disagg_update_file_meta --
 *     Update an existing file: entry in the local metadata table with checkpoint information from
 *     the shared metadata, then mark stale data handles as outdated.
 */
static int
__disagg_update_file_meta(
  WT_SESSION_IMPL *session, WT_CURSOR *sh_file_cursor, WT_CURSOR *md_file_cursor)
{
    WT_CONFIG_ITEM cval, cval_cur;
    WT_DECL_ITEM(metadata_cfg);
    WT_DECL_ITEM(old_uri_buf);
    WT_DECL_RET;
    char *cfg_ret, *current_value_copy;
    const char *cfg[3], *checkpoint_name, *current_value;
    const char *md_file_key, *metadata_value, *sh_file_key;
    bool discard;

    cfg_ret = current_value_copy = NULL;
    checkpoint_name = NULL;
    discard = false;

    WT_ERR(__wt_scr_alloc(session, 0, &metadata_cfg));
    WT_ERR(__wt_scr_alloc(session, 0, &old_uri_buf));
    WT_ERR(sh_file_cursor->get_key(sh_file_cursor, &sh_file_key));
    WT_ERR(sh_file_cursor->get_value(sh_file_cursor, &metadata_value));
    WT_ERR(__wt_config_getones(session, metadata_value, "checkpoint", &cval));
    WT_ERR(__wt_buf_fmt(session, metadata_cfg, "checkpoint=%.*s", (int)cval.len, cval.str));

    /* Check that the local metadata cursor is positioned at the same key. */
    WT_ERR(md_file_cursor->get_key(md_file_cursor, &md_file_key));
    WT_ASSERT(session, strcmp(md_file_key, sh_file_key) == 0);

    /* Merge the new checkpoint metadata into the current table metadata. */
    WT_ERR(md_file_cursor->get_value(md_file_cursor, &current_value));
    /* Copy the value since we don't own the memory after calling get_value(). */
    WT_ERR(__wt_strdup(session, current_value, &current_value_copy));

    /*
     * Extract the checkpoint information from the current value and compare it to the new
     * checkpoint. If they are the same, there is no need to proceed further.
     */
    WT_ERR(__wt_config_getones(session, current_value, "checkpoint", &cval_cur));
    if (__wt_string_slice_cmp(cval_cur.str, cval_cur.len, cval.str, cval.len) == 0)
        goto err;

    /* Overwrite the checkpoint field in the local metadata with the one from shared storage. */
    cfg[0] = current_value_copy;
    cfg[1] = metadata_cfg->data;
    cfg[2] = NULL;
    WT_ERR(__wt_config_collapse(session, cfg, &cfg_ret));

    md_file_cursor->set_value(md_file_cursor, cfg_ret);
    WT_ERR_MSG_CHK(session, md_file_cursor->update(md_file_cursor),
      "Failed to update metadata for key \"%s\"", sh_file_key);
    WT_STAT_CONN_INCR(session, disagg_pick_up_file_meta_updated);

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Updated the local metadata for key \"%s\" to include new checkpoint: \"%.*s\"", sh_file_key,
      (int)cval.len, cval.str);

    /*
     * Mark any matching data handles associated with the previous checkpoint to be out of date. Any
     * new opens will get the new metadata.
     *
     * FIXME-WT-14730: Check that the other parts of the metadata are identical.
     *
     * FIXME-WT-16494: How to decide two checkpoints are different if they are written by different
     * nodes?
     */
    WT_ERR(__disagg_discard_old_checkpoint_check(
      session, current_value_copy, cfg_ret, &checkpoint_name, &discard));
    if (discard) {
        WT_ERR(__wt_buf_fmt(session, old_uri_buf, "%s/%s", sh_file_key, checkpoint_name));
        WT_WITHOUT_DHANDLE(session, ret = __wti_conn_dhandle_outdated(session, old_uri_buf->data));
        WT_ERR_MSG_CHK(session, ret, "Marking data handles outdated failed: \"%s\"",
          (const char *)old_uri_buf->data);
    }

    /*
     * Mark all live btrees as outdated. Otherwise, we will not open a new dhandle for live btrees
     * after step-up.
     *
     * FIXME-WT-17772: This is better done at step-up or step-down to force close all live btrees.
     */
    WT_WITHOUT_DHANDLE(session, ret = __wti_conn_dhandle_outdated(session, sh_file_key));
    WT_ERR_MSG_CHK(session, ret, "Marking data handles outdated failed: \"%s\"", sh_file_key);

err:
    __wt_scr_free(session, &metadata_cfg);
    __wt_scr_free(session, &old_uri_buf);
    __wt_free(session, current_value_copy);
    __wt_free(session, cfg_ret);
    __wt_free(session, checkpoint_name);
    return (ret);
}

/*
 * __disagg_apply_checkpoint_meta --
 *     Process the metadata entries stored in the shared metadata table for a new checkpoint.
 */
static int
__disagg_apply_checkpoint_meta(WT_SESSION_IMPL *session, const WT_DISAGG_CHECKPOINT_META *ckpt_meta)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR *md_cursors[WT_DISAGG_CURSOR_COUNT], *md_write_cursor,
      *sh_cursors[WT_DISAGG_CURSOR_COUNT];
    WT_DECL_ITEM(current_buf);
    WT_DECL_ITEM(metadata_uri_buf);
    WT_DECL_RET;
    WT_TIMER apply_timer;
    uint64_t apply_elapsed_ms;
    uint32_t existing_tables, new_tables, new_ingest;
    size_t current_len;
    int i;
    char *layered_ingest_uri;
    const char *cfg[2], *metadata_checkpoint_name, *metadata_value;
    const char *md_keys[WT_DISAGG_CURSOR_COUNT], *sh_keys[WT_DISAGG_CURSOR_COUNT];
    const char *current;
    bool md_has[WT_DISAGG_CURSOR_COUNT], sh_has[WT_DISAGG_CURSOR_COUNT];

    for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++)
        md_cursors[i] = sh_cursors[i] = NULL;
    md_write_cursor = NULL;

    metadata_checkpoint_name = NULL;
    layered_ingest_uri = NULL;
    existing_tables = new_tables = new_ingest = 0;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->schema_lock);

    __wt_timer_start(session, &apply_timer);
    __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Processing new disaggregated storage checkpoint: metadata_lsn=%" PRIu64,
      ckpt_meta->metadata_lsn);

    /*
     * Look up the most recent checkpoint of the shared metadata table. If there is no checkpoint
     * yet (e.g. the shared metadata table has never been checkpointed or the database has empty
     * layered tables), there is no new checkpoint related configs to update. In that case return
     * success.
     */
    WT_ERR_NOTFOUND_OK(__wt_meta_checkpoint_last_name(
                         session, WT_DISAGG_METADATA_URI, &metadata_checkpoint_name, NULL, NULL),
      false);
    if (metadata_checkpoint_name == NULL)
        goto done;

    /*
     * !!!
     * Open four parallel cursor pairs - one pair per URI scheme (colgroup:, file:, layered:,
     * table:). Within each pair, md_cursors[i] scans local metadata and sh_cursors[i] scans the
     * latest checkpoint of the shared metadata table. Both cursors in a pair are bounded to their
     * respective URI scheme and advanced in lockstep so that entries for the same logical table
     * name are reconciled together.
     *
     * For example, a layered table "foo" produces entries across four schemes:
     *   colgroup:foo    file:foo.wt    layered:foo    table:foo
     *
     * The merge loop picks the minimum table name across all eight cursor positions in each
     * iteration, then processes all four local/shared pairs for that name before advancing.
     */

    /* Open the metadata cursors on the local metadata table. */
    for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++)
        WT_ERR(__wt_metadata_cursor(session, &md_cursors[i]));
    WT_ERR(__wt_metadata_cursor(session, &md_write_cursor));

    /* Open the cursors on the shared metadata table. */
    WT_ERR(__wt_scr_alloc(session, 0, &current_buf));
    WT_ERR(__wt_scr_alloc(session, 0, &metadata_uri_buf));
    WT_ERR(__wt_buf_fmt(
      session, metadata_uri_buf, "%s/%s", WT_DISAGG_METADATA_URI, metadata_checkpoint_name));

    cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_open_cursor);
    cfg[1] = NULL;

    for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++)
        WT_ERR(__wt_open_cursor(session, metadata_uri_buf->data, NULL, cfg, &sh_cursors[i]));

    /* Position the cursors by setting the lower and upper bounds. */
    for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++) {
        WT_ERR(__disagg_bound_cursor(session, md_cursors[i], __disagg_cursor_prefixes[i]));
        WT_ERR(__disagg_bound_cursor(session, sh_cursors[i], __disagg_cursor_prefixes[i]));
    }

    /*
     * Initialize the cursor state arrays so that the first iteration calls next on every cursor.
     * Calling next on a cursor with no position moves it to the first entry within its bounds.
     */
    for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++) {
        md_has[i] = sh_has[i] = true;
        md_keys[i] = sh_keys[i] = NULL;
    }

    for (;;) {

        /* Advance the cursors that are positioned at the current table name. */
        for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++) {
            if (md_has[i])
                WT_ERR(__disagg_cursor_next(md_cursors[i], &md_keys[i]));
            if (sh_has[i])
                WT_ERR(__disagg_cursor_next(sh_cursors[i], &sh_keys[i]));
        }
        WT_ERR(__disagg_file_skip_local(
          md_cursors[WT_DISAGG_CURSOR_FILE], &md_keys[WT_DISAGG_CURSOR_FILE]));

        /*
         * Find the minimum table name across all non-exhausted cursors. Entries for the same
         * logical table share a name across URI schemes (e.g. "table:foo", "file:foo.wt",
         * "layered:foo") and are processed together in one iteration.
         */
        current = NULL;
        current_len = 0;
        for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++) {
            __disagg_update_min(md_keys[i], i, &current, &current_len);
            __disagg_update_min(sh_keys[i], i, &current, &current_len);
        }

        /* All cursors are exhausted. */
        if (current == NULL)
            break;

        /* Copy and zero-terminate the table name. */
        WT_ERR(__wt_buf_set(session, current_buf, current, current_len + 1));
        ((char *)current_buf->data)[current_len] = '\0';
        current = current_buf->data;

        /* Mark which cursors are positioned at the current table name. */
        for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++) {
            md_has[i] = __disagg_key_at_table(md_keys[i], i, current, current_len);
            sh_has[i] = __disagg_key_at_table(sh_keys[i], i, current, current_len);
        }

        /* Log the reconciliation state for this table across all URI schemes. */
        if (WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_DISAGGREGATED_STORAGE, WT_VERBOSE_DEBUG_2)) {
            __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Reconciling metadata for \"%s\": "
              "Local=[colgroup:%s file:%s layered:%s table:%s] "
              "Shared=[colgroup:%s file:%s layered:%s table:%s]",
              current, md_has[WT_DISAGG_CURSOR_COLGROUP] ? "Y" : "N",
              md_has[WT_DISAGG_CURSOR_FILE] ? "Y" : "N",
              md_has[WT_DISAGG_CURSOR_LAYERED] ? "Y" : "N",
              md_has[WT_DISAGG_CURSOR_TABLE] ? "Y" : "N",
              sh_has[WT_DISAGG_CURSOR_COLGROUP] ? "Y" : "N",
              sh_has[WT_DISAGG_CURSOR_FILE] ? "Y" : "N",
              sh_has[WT_DISAGG_CURSOR_LAYERED] ? "Y" : "N",
              sh_has[WT_DISAGG_CURSOR_TABLE] ? "Y" : "N");
            for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++) {
                if (md_has[i]) {
                    WT_ERR(md_cursors[i]->get_value(md_cursors[i], &metadata_value));
                    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "  Local [%s]: %s",
                      __disagg_cursor_prefixes[i], metadata_value);
                }
                if (sh_has[i]) {
                    WT_ERR(sh_cursors[i]->get_value(sh_cursors[i], &metadata_value));
                    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "  Shared[%s]: %s",
                      __disagg_cursor_prefixes[i], metadata_value);
                }
            }
        }

        /*
         * Reconcile entries for this URI scheme and table.
         */
        if (md_has[WT_DISAGG_CURSOR_LAYERED] && sh_has[WT_DISAGG_CURSOR_LAYERED]) {
            /*
             * Both the local and shared metadata tables have a layered: entry. Update the file:
             * entry's checkpoint information and mark any stale data handles outdated.
             */
            if (!sh_has[WT_DISAGG_CURSOR_FILE])
                WT_ERR_MSG(session, EINVAL,
                  "Missing shared file: metadata entry for layered table \"%s\"", current);
            if (md_has[WT_DISAGG_CURSOR_FILE])
                /*
                 * The file already exists in the local metadata, so we just pick up its latest
                 * checkpoint without changing its other metadata.
                 */
                WT_ERR(__disagg_update_file_meta(
                  session, sh_cursors[WT_DISAGG_CURSOR_FILE], md_cursors[WT_DISAGG_CURSOR_FILE]));
            else
                /*
                 * We already have the layered table in the local metadata; we are just picking up
                 * the stable component.
                 */
                WT_ERR(__disagg_insert_meta(
                  session, sh_cursors[WT_DISAGG_CURSOR_FILE], md_write_cursor));
            ++existing_tables;
        } else if (!md_has[WT_DISAGG_CURSOR_LAYERED] && sh_has[WT_DISAGG_CURSOR_LAYERED]) {
            /*
             * The shared metadata has a layered: entry but the local metadata does not. This could
             * be a new layered table that we should pick up, but it could also mean that we have
             * already dropped the table locally and should not recreate it as a result.
             */
            if (__wti_disagg_table_latest_create_remove(session, current) ==
              WT_SHARED_METADATA_REMOVE)
                continue;

            /*
             * This is a new layered table. Create the ingest table if needed, then copy all shared
             * entries.
             */
            if (!sh_has[WT_DISAGG_CURSOR_FILE])
                WT_ERR_MSG(session, EINVAL,
                  "Missing shared file: metadata entry for new layered table \"%s\"", current);
            if (md_has[WT_DISAGG_CURSOR_FILE] || md_has[WT_DISAGG_CURSOR_COLGROUP] ||
              md_has[WT_DISAGG_CURSOR_TABLE])
                WT_ERR_MSG(session, EINVAL,
                  "Unexpected local metadata entries for new layered table \"%s\"", current);

            /* FIXME-WT-14730: Verify that there is no btree ID conflict. */
            WT_ERR(sh_cursors[WT_DISAGG_CURSOR_LAYERED]->get_value(
              sh_cursors[WT_DISAGG_CURSOR_LAYERED], &metadata_value));
            WT_ERR(__wt_config_getones(session, metadata_value, "ingest", &cval));
            if (cval.len > 0) {
                WT_ERR(__wt_calloc_def(session, cval.len + 1, &layered_ingest_uri));
                memcpy(layered_ingest_uri, cval.str, cval.len);
                layered_ingest_uri[cval.len] = '\0';
                md_write_cursor->set_key(md_write_cursor, layered_ingest_uri);
                WT_ERR_NOTFOUND_OK(md_write_cursor->search(md_write_cursor), true);
                if (ret == WT_NOTFOUND) {
                    WT_ERR_MSG_CHK(session,
                      __layered_create_missing_ingest_table(
                        session, layered_ingest_uri, metadata_value),
                      "Failed to create missing ingest table \"%s\" from \"%s\"",
                      layered_ingest_uri, metadata_value);
                    ++new_ingest;
                }
                __wt_free(session, layered_ingest_uri);
                layered_ingest_uri = NULL;
            }
            WT_ERR(
              __disagg_insert_meta(session, sh_cursors[WT_DISAGG_CURSOR_LAYERED], md_write_cursor));
            WT_ERR(
              __disagg_insert_meta(session, sh_cursors[WT_DISAGG_CURSOR_FILE], md_write_cursor));
            if (sh_has[WT_DISAGG_CURSOR_COLGROUP])
                WT_ERR(__disagg_insert_meta(
                  session, sh_cursors[WT_DISAGG_CURSOR_COLGROUP], md_write_cursor));
            if (sh_has[WT_DISAGG_CURSOR_TABLE])
                WT_ERR(__disagg_insert_meta(
                  session, sh_cursors[WT_DISAGG_CURSOR_TABLE], md_write_cursor));
            ++new_tables;
        } else if (md_has[WT_DISAGG_CURSOR_LAYERED] && !sh_has[WT_DISAGG_CURSOR_LAYERED]) {
            /*
             * The local metadata has a layered: entry but the shared metadata does not - a dropped
             * layered table.
             *
             * FIXME-WT-17746: Remove the local metadata entries for the dropped table.
             */
        } else {
            /*
             * Neither the local nor the shared metadata has a layered: entry for this table name.
             * This is the normal path for non-layered tables in shared storage and for file-only
             * disaggregated entries.
             */

            /* Skip the shared metadata file, as it has already been processed. */
            if (sh_has[WT_DISAGG_CURSOR_FILE] &&
              strcmp(sh_keys[WT_DISAGG_CURSOR_FILE], WT_DISAGG_METADATA_URI) == 0)
                continue;

            /*
             * Process any table: entries for shared non-layered tables and for local-only tables
             * This is uncommon, but it is used in WiredTiger's testing.
             */
            if (sh_has[WT_DISAGG_CURSOR_TABLE] && !md_has[WT_DISAGG_CURSOR_TABLE])
                /*
                 * Insert the shared table metadata into the local metadata. We could end up here if
                 * the leader created a table: object without the corresponding layered: object by
                 * specifying the disagg block manager.
                 *
                 * We do not check the metadata operations queue as we do for layered tables,
                 * because we don't currently support the publish API for non-layered tables.
                 */
                WT_ERR(__disagg_insert_meta(
                  session, sh_cursors[WT_DISAGG_CURSOR_TABLE], md_write_cursor));
            else if (!sh_has[WT_DISAGG_CURSOR_TABLE] && md_has[WT_DISAGG_CURSOR_TABLE])
                /*
                 * The local metadata has a table: entry but the shared metadata does not. This
                 * happens for local (non-disaggregated) tables. We could also end up here if the
                 * leader dropped a shared non-layered table. We currently don't handle this case.
                 */
                __wt_verbose_debug3(session, WT_VERB_DISAGGREGATED_STORAGE,
                  "Local table metadata for \"%s\" has no corresponding shared metadata", current);

            /*
             * Insert any colgroup: entries that are in the shared metadata but not yet in the local
             * metadata. This is likewise uncommon, but it is used in WiredTiger's testing.
             *
             * If a table has more than one column group, it may arrive across multiple iterations.
             * This is not supported by the publish API, but we should still handle it gracefully.
             */
            if (sh_has[WT_DISAGG_CURSOR_COLGROUP] && !md_has[WT_DISAGG_CURSOR_COLGROUP])
                WT_ERR(__disagg_insert_meta(
                  session, sh_cursors[WT_DISAGG_CURSOR_COLGROUP], md_write_cursor));

            /*
             * Update the file: entry's checkpoint information and insert any entries that are in
             * the shared metadata but not yet in the local metadata. This is, for example, how we
             * handle the shared history store, which is a file: object without the corresponding
             * table: or layered: object.
             */
            if (sh_has[WT_DISAGG_CURSOR_FILE] && !md_has[WT_DISAGG_CURSOR_FILE]) {
                /*
                 * The shared metadata table has an entry for this file. Add it to the local
                 * metadata.
                 */
                WT_ERR(__disagg_insert_meta(
                  session, sh_cursors[WT_DISAGG_CURSOR_FILE], md_write_cursor));
                ++new_tables;
            } else if (sh_has[WT_DISAGG_CURSOR_FILE] && md_has[WT_DISAGG_CURSOR_FILE]) {
                /*
                 * Both the shared and local metadata tables have an entry for this file. Update the
                 * local metadata with any new checkpoint information from the shared metadata, and
                 * mark any old checkpoints as discarded.
                 */
                WT_ERR(__disagg_update_file_meta(
                  session, sh_cursors[WT_DISAGG_CURSOR_FILE], md_cursors[WT_DISAGG_CURSOR_FILE]));
                ++existing_tables;
            } else if (!sh_has[WT_DISAGG_CURSOR_FILE] && md_has[WT_DISAGG_CURSOR_FILE])
                /*
                 * The local metadata has an entry for this file, but the shared metadata does not.
                 * This happens for local (non-disaggregated) tables and btrees. Note that we should
                 * not hit this case for the ingest components of layered tables, because we skipped
                 * them right after advancing the cursors above.
                 *
                 * We could also end up here if the leader dropped a shared non-layered table or a
                 * btree. We currently don't handle this case.
                 */
                __wt_verbose_debug3(session, WT_VERB_DISAGGREGATED_STORAGE,
                  "Local file metadata for \"%s\" has no corresponding shared metadata", current);
        }
    }

    __wt_timer_evaluate_ms(session, &apply_timer, &apply_elapsed_ms);
    WT_STAT_CONN_SET(session, disagg_apply_checkpoint_meta_time, apply_elapsed_ms);
    __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Checkpoint pickup processed %" PRIu32 " existing tables, %" PRIu32 " new tables, %" PRIu32
      " new ingest tables in %" PRIu64 "ms",
      existing_tables, new_tables, new_ingest, apply_elapsed_ms);

done:
err:
    __wt_free(session, metadata_checkpoint_name);
    __wt_free(session, layered_ingest_uri);
    __wt_scr_free(session, &current_buf);
    __wt_scr_free(session, &metadata_uri_buf);

    WT_TRET(__wt_metadata_cursor_release(session, &md_write_cursor));
    for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++) {
        WT_TRET(__wt_metadata_cursor_release(session, &md_cursors[i]));
        if (sh_cursors[i] != NULL)
            WT_TRET(sh_cursors[i]->close(sh_cursors[i]));
    }

    return (ret);
}

/*
 * __raise_next_file_id --
 *     Increase our next file ID if necessary. This value is only important for synchronizing
 *     changes to the shared metadata table, which are made only by the leader. The increment only
 *     happens on a follower, which will make tables only in response to the leader (via picking up
 *     a checkpoint, or by oplog application). So it's OK if we've made new files since this
 *     checkpoint was generated.
 */
static void
__raise_next_file_id(WT_SESSION_IMPL *session, const WT_DISAGG_METADATA *metadata)
{
    WT_CONNECTION_IMPL *conn = S2C(session);

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->schema_lock);

    if (conn->next_file_id < metadata->largest_file_id)
        conn->next_file_id = metadata->largest_file_id;
}

/*
 * __disagg_finalize_checkpoint_meta --
 *     Finalize checkpoint bookkeeping after processing shared metadata entries.
 */
static int
__disagg_finalize_checkpoint_meta(WT_SESSION_IMPL *session,
  const WT_DISAGG_CHECKPOINT_META *ckpt_meta, const WT_DISAGG_METADATA *metadata)
{
    WT_DECL_RET;
    WT_CONNECTION_IMPL *conn = S2C(session);

    /*
     * Update the checkpoint metadata LSN. This doesn't require further synchronization, because the
     * updates are protected by the checkpoint lock.
     */
    __wt_atomic_store_uint64_release(
      &conn->disaggregated_storage.last_checkpoint_meta_lsn, ckpt_meta->metadata_lsn);

    /* Update the timestamps. */
    __wt_atomic_store_uint64_release(
      &conn->disaggregated_storage.last_checkpoint_schema_epoch, metadata->schema_epoch);
    __wt_atomic_store_uint64_release(
      &conn->disaggregated_storage.last_checkpoint_timestamp, metadata->checkpoint_timestamp);
    __wt_atomic_store_uint64_release(
      &conn->disaggregated_storage.last_checkpoint_oldest_timestamp, metadata->oldest_timestamp);
    conn->txn_global.last_ckpt_disaggregated_schema_epoch = metadata->schema_epoch;
    conn->txn_global.last_ckpt_timestamp = metadata->checkpoint_timestamp;

    /* Set the database size. */
    __wt_disagg_set_database_size(session, ckpt_meta->database_size);

    /* Remember the root config of the last checkpoint. */
    __wt_free(session, conn->disaggregated_storage.last_checkpoint_root);
    WT_ERR(__wt_strndup(session, metadata->checkpoint, metadata->checkpoint_len,
      &conn->disaggregated_storage.last_checkpoint_root));

    /* Update ingest tables' prune timestamps. */
    WT_ERR_MSG_CHK(session,
      __wti_layered_iterate_ingest_tables_for_gc_pruning(session, metadata->checkpoint_timestamp),
      "Updating prune timestamp failed");

    WT_WITH_SCHEMA_LOCK(session, __raise_next_file_id(session, metadata));

err:
    return (ret);
}

/*
 * __disagg_pick_up_checkpoint --
 *     Pick up a new checkpoint.
 */
static int
__disagg_pick_up_checkpoint(WT_SESSION_IMPL *session, const WT_DISAGG_CHECKPOINT_META *ckpt_meta)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGG_METADATA metadata;
    WT_ITEM metadata_buf;
    WT_TIMER pickup_timer;
    uint64_t current_meta_lsn, pickup_elapsed_ms;
    char ts_string[3][WT_TS_INT_STRING_SIZE];

    conn = S2C(session);

    WT_CLEAR(ts_string);
    WT_CLEAR(metadata_buf);
    WT_CLEAR(metadata);

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    /*
     * Reset the statistics tracked per checkpoint. Technically this isn't a checkpoint but we
     * should reset the statistics so they are still useful.
     */
    __wt_checkpoint_reset_stats(conn);

    /* We should not pick up a checkpoint with an earlier LSN. */
    current_meta_lsn =
      __wt_atomic_load_uint64_acquire(&conn->disaggregated_storage.last_checkpoint_meta_lsn);
    if (ckpt_meta->metadata_lsn < current_meta_lsn)
        WT_RET_MSG(session, EINVAL,
          "Attempting to pick up an older checkpoint: current metadata LSN = %" PRIu64
          ", new metadata LSN = %" PRIu64,
          current_meta_lsn, ckpt_meta->metadata_lsn);

    /*
     * Warn if we are picking up the same checkpoint again. There's nothing else to do here, goto
     * err for cleanup.
     */
    if (ckpt_meta->metadata_lsn == current_meta_lsn) {
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_WARNING,
          "Picking up the same checkpoint again: metadata LSN = %" PRIu64, ckpt_meta->metadata_lsn);
        /* Keep previous ret value to avoid overlapping error message */
        goto err;
    }

    __wt_timer_start(session, &pickup_timer);
    __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Picking up disaggregated storage checkpoint: metadata_lsn=%" PRIu64,
      ckpt_meta->metadata_lsn);

    /*
     * Part 1: Get the metadata of the shared metadata table and insert it into our metadata table.
     */

    WT_ERR(__wti_disagg_fetch_shared_meta(session, ckpt_meta, &metadata_buf));
    WT_ERR(__wt_disagg_parse_meta(session, &metadata_buf, &metadata));

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Picking up disaggregated storage checkpoint: metadata_lsn=%" PRIu64 ", timestamp=%" PRIu64
      " %s, oldest_timestamp=%" PRIu64 " %s, schema_epoch=%" PRIu64 " %s, largest_file_id=%" PRIu32
      ", root=\"%.*s\"",
      ckpt_meta->metadata_lsn, metadata.checkpoint_timestamp,
      __wt_timestamp_to_string(metadata.checkpoint_timestamp, ts_string[0]),
      metadata.oldest_timestamp, __wt_timestamp_to_string(metadata.oldest_timestamp, ts_string[1]),
      metadata.schema_epoch, __wt_timestamp_to_string(metadata.schema_epoch, ts_string[2]),
      metadata.largest_file_id, (int)metadata.checkpoint_len, metadata.checkpoint);

    /* Load crypt key data with the key provider extension, if any. */
    WT_ERR(__wti_disagg_load_crypt_key(session, &metadata));

    /* Update our local metadata with the new checkpoint entry. */
    WT_ERR(__disagg_save_checkpoint_meta_local(session, &metadata));

    /*
     * Part 2: Apply the metadata for other tables from the shared metadata table.
     */

    /* Apply the metadata from the checkpoint. */
    WT_WITH_SCHEMA_LOCK(session, ret = __disagg_apply_checkpoint_meta(session, ckpt_meta));
    WT_ERR(ret);

    /*
     * Part 3: Do the bookkeeping.
     */

    __wti_disagg_shared_metadata_queue_prune(session, metadata.schema_epoch);
    WT_ERR(__disagg_finalize_checkpoint_meta(session, ckpt_meta, &metadata));

    /* Log the completion of the checkpoint pick-up. */
    __wt_timer_evaluate_ms(session, &pickup_timer, &pickup_elapsed_ms);
    WT_STAT_CONN_SET(session, disagg_pick_up_checkpoint_time, pickup_elapsed_ms);
    __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Finished picking up disaggregated storage checkpoint: metadata_lsn=%" PRIu64 " in %" PRIu64
      "ms",
      ckpt_meta->metadata_lsn, pickup_elapsed_ms);

err:
    if (ret == 0) {
        WT_STAT_CONN_INCR(session, layered_table_manager_checkpoints_disagg_pick_up_succeed);
        if (!conn->layered_table_manager.leader)
            WT_STAT_CONN_INCR(session, layered_table_manager_checkpoints_disagg_pick_up_follower);
    } else {
        WT_STAT_CONN_INCR(session, layered_table_manager_checkpoints_disagg_pick_up_failed);
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_ERROR,
          "Failed to pick up disaggregated storage checkpoint for metadata_lsn=%" PRIu64 ": ret=%d",
          ckpt_meta->metadata_lsn, ret);
    }

    __wt_buf_free(session, &metadata_buf);

    return (ret);
}

/*
 * __disagg_check_meta_version --
 *     Parse and validate version and compatible_version fields from checkpoint metadata config.
 *     Populates the version and compatible_version fields in ckpt_meta struct.
 */
static int
__disagg_check_meta_version(
  WT_SESSION_IMPL *session, const char *meta_str, WT_DISAGG_CHECKPOINT_META *ckpt_meta)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;

    /* Initialize to defaults for backward compatibility (missing version fields). */
    ckpt_meta->version = WT_DISAGG_CHECKPOINT_META_VERSION_DEFAULT;
    ckpt_meta->compatible_version = WT_DISAGG_CHECKPOINT_META_VERSION_DEFAULT;

    WT_ERR_NOTFOUND_OK(__wt_config_getones(session, meta_str, "version", &cval), true);
    if (ret == 0 && cval.len != 0) {
        if (cval.val > UINT32_MAX)
            WT_ERR_MSG(
              session, EINVAL, "Invalid checkpoint_meta version: %" PRIu64, (uint64_t)cval.val);
        ckpt_meta->version = (uint32_t)cval.val;
    }

    WT_ERR_NOTFOUND_OK(__wt_config_getones(session, meta_str, "compatible_version", &cval), true);
    if (ret == 0 && cval.len != 0) {
        if (cval.val > UINT32_MAX)
            WT_ERR_MSG(session, EINVAL, "Invalid checkpoint_meta compatible_version: %" PRIu64,
              (uint64_t)cval.val);
        ckpt_meta->compatible_version = (uint32_t)cval.val;
    }

    /* Clear error status (WT_NOTFOUND is ok for optional fields, means use default). */
    ret = 0;

    /* Check if this checkpoint metadata is compatible with the current reader version. */
    if (ckpt_meta->compatible_version > WT_DISAGG_CHECKPOINT_META_VERSION)
        WT_ERR_MSG(session, ENOTSUP,
          "Checkpoint meta compatible_version=%" PRIu32 " requires reader version >= %d",
          ckpt_meta->compatible_version, WT_DISAGG_CHECKPOINT_META_VERSION);

    if (ckpt_meta->version < ckpt_meta->compatible_version)
        WT_ERR_MSG(session, EINVAL,
          "Illegal version: Checkpoint meta version=%" PRIu32
          " is older than compatible_version=%" PRIu32,
          ckpt_meta->version, ckpt_meta->compatible_version);

err:
    return (ret);
}

/*
 * __wti_disagg_pick_up_checkpoint_meta --
 *     Pick up a new checkpoint from metadata config.
 */
int
__wti_disagg_pick_up_checkpoint_meta(
  WT_SESSION_IMPL *session, const char *meta_data, size_t meta_data_size)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_DISAGG_CHECKPOINT_META ckpt_meta;
    WT_SESSION_IMPL *internal_session;
    uint64_t metadata_checksum;
    char *meta_str;

    WT_CLEAR(ckpt_meta);
    meta_str = NULL;
    internal_session = NULL;

    /* Extract the item into a string. */
    WT_ERR(__wt_strndup(session, meta_data, meta_data_size, &meta_str));

    /* Extract the LSN of the metadata page. */
    WT_ERR(__wt_config_getones(session, meta_str, "metadata_lsn", &cval));
    ckpt_meta.metadata_lsn = (uint64_t)cval.val;

    /*
     * Extract the checksum of the metadata page, if it exists. We added the checksum later, so
     * treat it as optional, in order to support clusters with an earlier data format.
     */
    WT_ERR_NOTFOUND_OK(__wt_config_getones(session, meta_str, "metadata_checksum", &cval), true);
    if (ret == 0 && cval.len != 0) {
        WT_ERR(__wt_conf_parse_hex(session, "metadata_checksum", &metadata_checksum, &cval));
        if (metadata_checksum > UINT32_MAX)
            WT_ERR_MSG(
              session, EINVAL, "Invalid metadata checksum value: %" PRIx64, metadata_checksum);
        ckpt_meta.has_metadata_checksum = true;
        ckpt_meta.metadata_checksum = (uint32_t)metadata_checksum;
    } else
        /* FIXME-WT-16000: Make the checksum parameter in "checkpoint_meta" required */
        __wt_verbose_warning(session, WT_VERB_DISAGGREGATED_STORAGE, "%s\"%s\"",
          "Missing metadata_checksum from metadata: ", meta_str);

    /* Extract the database size. */
    WT_ERR(__wt_config_getones(session, meta_str, "database_size", &cval));
    ckpt_meta.database_size = (uint64_t)cval.val;
    /* Parse and validate version and compatible_version fields. */
    WT_ERR(__disagg_check_meta_version(session, meta_str, &ckpt_meta));

    WT_ERR(__wt_open_internal_session(
      S2C(session), "checkpoint-pick-up", false, 0, 0, &internal_session));
    /* Now actually pick up the checkpoint. */
    WT_WITH_CHECKPOINT_LOCK(
      internal_session, ret = __disagg_pick_up_checkpoint(internal_session, &ckpt_meta));
    WT_ERR(ret);

err:
    if (internal_session != NULL)
        WT_TRET(__wt_session_close_internal(internal_session));
    __wt_free(session, meta_str);
    return (ret);
}

#ifdef HAVE_UNITTEST
int
__ut_disagg_validate_checkpoint_meta_version(WT_SESSION_IMPL *session, const char *meta_str,
  uint32_t *out_version, uint32_t *out_compatible_version)
{
    WT_DISAGG_CHECKPOINT_META ckpt_meta;

    /* Set default test value */
    *out_version = 0;
    *out_compatible_version = 0;

    /* Initialize struct with defaults */
    memset(&ckpt_meta, 0, sizeof(ckpt_meta));

    /* Call the main version check function */
    WT_RET(__disagg_check_meta_version(session, meta_str, &ckpt_meta));

    /* Return parsed values */
    *out_version = ckpt_meta.version;
    *out_compatible_version = ckpt_meta.compatible_version;

    return (0);
}
#endif
