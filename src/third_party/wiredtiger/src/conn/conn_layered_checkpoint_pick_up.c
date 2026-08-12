/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
static int __disagg_check_meta_fields(
  WT_SESSION_IMPL *, const char *, const char *, WT_CONFIG *, WT_CONFIG *);
#endif

static int __disagg_adopt_deferred_checkpoint_meta(WT_SESSION_IMPL *, const char *, size_t);

/*
 * __layered_create_missing_ingest_table --
 *     Create a missing ingest table from an existing layered table configuration.
 */
static int
__layered_create_missing_ingest_table(
  WT_SESSION_IMPL *session, const char *uri, const char *layered_cfg, bool is_startup)
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

    /*
     * On the first checkpoint pickup, skip opening a dhandle for each newly created ingest table:
     * the cost scales with the number of tables and dominates startup time and the dhandle will be
     * opened on first access instead. This is safe because ingest tables are in-memory only and
     * skipped by checkpoints. Steady-state pickups create few tables, so keep the eager open there
     * to avoid acquiring the schema lock again on first access.
     */
    WT_WITH_SCHEMA_LOCK(
      session, ret = __wt_schema_create_internal(session, uri, ingest_config->data, !is_startup));

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
 * __disagg_replace_checkpoint --
 *     Rebuild a metadata config string, substituting only the last checkpoint= value. Earlier
 *     duplicates are left unchanged.
 */
static int
__disagg_replace_checkpoint(
  WT_SESSION_IMPL *session, const char *base, const WT_CONFIG_ITEM *new_ckpt, char **config_ret)
{
    WT_CONFIG cparser;
    WT_CONFIG_ITEM k, last_ckpt_key, v;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;

    *config_ret = NULL;
    WT_CLEAR(last_ckpt_key);

    /* Find the last checkpoint= key; config lookups already take that final match. */
    __wt_config_init(session, &cparser, base);
    while ((ret = __wt_config_next(&cparser, &k, &v)) == 0) {
        if (k.type != WT_CONFIG_ITEM_STRING && k.type != WT_CONFIG_ITEM_ID)
            WT_RET_MSG(
              session, EINVAL, "Invalid configuration key found: '%.*s'", (int)k.len, k.str);
        if (WT_CONFIG_LIT_MATCH("checkpoint", k))
            last_ckpt_key = k;
    }
    WT_RET_NOTFOUND_OK(ret);
    if (last_ckpt_key.str == NULL)
        return (WT_NOTFOUND);

    WT_RET(__wt_scr_alloc(session, strlen(base) + new_ckpt->len + 32, &tmp));

    __wt_config_init(session, &cparser, base);
    while ((ret = __wt_config_next(&cparser, &k, &v)) == 0) {
        if (k.type != WT_CONFIG_ITEM_STRING && k.type != WT_CONFIG_ITEM_ID)
            WT_ERR_MSG(
              session, EINVAL, "Invalid configuration key found: '%.*s'", (int)k.len, k.str);
        if (k.str == last_ckpt_key.str)
            v = *new_ckpt;
        else {
            if (k.type == WT_CONFIG_ITEM_STRING)
                WT_CONFIG_PRESERVE_QUOTES(session, &k);
            if (v.type == WT_CONFIG_ITEM_STRING)
                WT_CONFIG_PRESERVE_QUOTES(session, &v);
        }
        WT_ERR(__wt_buf_catfmt(session, tmp, "%.*s=%.*s,", (int)k.len, k.str, (int)v.len, v.str));
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    /* Each entry was emitted with a trailing comma; drop the final one. */
    --tmp->size;
    WT_ERR(__wt_strndup(session, tmp->data, tmp->size, config_ret));

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __disagg_save_checkpoint_meta_local --
 *     Update the local metadata entry with the supplied checkpoint configuration.
 */
static int
__disagg_save_checkpoint_meta_local(WT_SESSION_IMPL *session, const WT_DISAGG_METADATA *metadata)
{
    WT_CONFIG_ITEM new_ckpt = {
      metadata->checkpoint, metadata->checkpoint_len, 0, WT_CONFIG_ITEM_STRUCT};
    WT_CURSOR *md_cursor;
    WT_DECL_ITEM(old_uri_buf);
    WT_DECL_RET;
    char *cfg_current_copy, *cfg_new;
    const char *checkpoint_name, *cfg_current, *metadata_key;
    bool discard;

    cfg_current_copy = cfg_new = NULL;
    checkpoint_name = NULL;
    discard = false;
    md_cursor = NULL;
    metadata_key = WT_DISAGG_METADATA_URI;

    /*
     * Open a private metadata cursor, leaving the session's cached one free for the tracked
     * metadata update below.
     */
    WT_ERR(__wt_metadata_cursor_open(session, NULL, &md_cursor));

    /* Pull the value out. */
    md_cursor->set_key(md_cursor, metadata_key);
    WT_ERR(md_cursor->search(md_cursor));
    WT_ERR(md_cursor->get_value(md_cursor, &cfg_current));
    /* Copy the value since we don't own the memory after calling get_value(). */
    WT_ERR(__wt_strdup(session, cfg_current, &cfg_current_copy));

    WT_ERR(__disagg_replace_checkpoint(session, cfg_current_copy, &new_ckpt, &cfg_new));

    /* Put in our new config: a tracked update, so a failed merge unrolls it. */
    WT_ERR(__wt_metadata_update(session, metadata_key, cfg_new));

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
    __wt_scr_free(session, &old_uri_buf);

    if (md_cursor != NULL)
        WT_TRET(md_cursor->close(md_cursor));
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

#ifdef HAVE_DIAGNOSTIC
/*
 * __disagg_meta_skip_field --
 *     Return true if the configuration field is excluded from the metadata comparison: it either
 *     legitimately changes across checkpoints, holds node-local state, or can be changed at runtime
 *     via WT_SESSION::alter. The list holds top-level field names only, sorted alphabetically.
 */
static bool
__disagg_meta_skip_field(const WT_CONFIG_ITEM *key)
{
    static const char *const skip[] = {"access_pattern_hint", "app_metadata", "assert",
      "cache_resident", "checkpoint", "checkpoint_backup_info", "checkpoint_lsn", "live_restore",
      "log", "os_cache_dirty_max", "os_cache_max", "verbose", "write_timestamp_usage", NULL};
    u_int i;
    int cmp;

    for (i = 0; skip[i] != NULL; i++) {
        cmp = __wt_string_slice_cmp(key->str, key->len, skip[i], strlen(skip[i]));
        if (cmp == 0)
            return (true);
        /* The list is sorted: no later entry can match a key that sorts before this one. */
        if (cmp < 0)
            return (false);
    }
    return (false);
}

/*
 * __disagg_check_meta_field --
 *     Compare a single configuration field present in both the local and the shared metadata,
 *     descending into nested categories and panicking when the types or the values differ.
 */
static int
__disagg_check_meta_field(WT_SESSION_IMPL *session, const char *uri, const char *prefix,
  const WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *md_cval, WT_CONFIG_ITEM *sh_cval)
{
    WT_CONFIG md_sub, sh_sub;
    WT_DECL_ITEM(child);
    WT_DECL_RET;

    WT_ASSERT_ALWAYS(session, sh_cval->type == md_cval->type,
      "checkpoint pickup metadata mismatch for \"%s\": the type of \"%s%.*s\" differs between the "
      "local and the shared metadata",
      uri, prefix, (int)key->len, key->str);
    if (sh_cval->type == WT_CONFIG_ITEM_STRUCT) {
        WT_ERR(__wt_scr_alloc(session, 0, &child));
        WT_ERR(__wt_buf_fmt(session, child, "%s%.*s.", prefix, (int)key->len, key->str));
        __wt_config_subinit(session, &sh_sub, sh_cval);
        __wt_config_subinit(session, &md_sub, md_cval);
        WT_ERR(__disagg_check_meta_fields(session, uri, child->data, &md_sub, &sh_sub));
    } else if (__wt_string_slice_cmp(sh_cval->str, sh_cval->len, md_cval->str, md_cval->len) != 0)
        WT_ERR_PANIC(session, EINVAL,
          "checkpoint pickup metadata mismatch for \"%s\": the value of \"%s%.*s\" differs "
          "between the local (\"%.*s\") and the shared (\"%.*s\") metadata",
          uri, prefix, (int)key->len, key->str, (int)md_cval->len, md_cval->str, (int)sh_cval->len,
          sh_cval->str);

err:
    __wt_scr_free(session, &child);
    return (ret);
}

/*
 * __disagg_check_meta_fields --
 *     Walk two configuration strings in lockstep, panicking when a field present on both sides has
 *     different values. Both strings must list their fields in sorted order, a field present on
 *     only one side is ignored.
 */
static int
__disagg_check_meta_fields(WT_SESSION_IMPL *session, const char *uri, const char *prefix,
  WT_CONFIG *md_parser, WT_CONFIG *sh_parser)
{
    WT_CONFIG_ITEM md_ckey, md_cval, sh_ckey, sh_cval;
    int cmp, md_ret, sh_ret;
    bool top_level;

    top_level = prefix[0] == '\0';
    sh_ret = __wt_config_next(sh_parser, &sh_ckey, &sh_cval);
    md_ret = __wt_config_next(md_parser, &md_ckey, &md_cval);
    while (sh_ret == 0 && md_ret == 0) {
        cmp = __wt_string_slice_cmp(sh_ckey.str, sh_ckey.len, md_ckey.str, md_ckey.len);
        if (cmp < 0)
            sh_ret = __wt_config_next(sh_parser, &sh_ckey, &sh_cval);
        else if (cmp > 0)
            md_ret = __wt_config_next(md_parser, &md_ckey, &md_cval);
        else {
            /* The skip list names top-level fields only. */
            if (!top_level || !__disagg_meta_skip_field(&sh_ckey))
                WT_RET(
                  __disagg_check_meta_field(session, uri, prefix, &sh_ckey, &md_cval, &sh_cval));

            sh_ret = __wt_config_next(sh_parser, &sh_ckey, &sh_cval);
            md_ret = __wt_config_next(md_parser, &md_ckey, &md_cval);
        }
    }

    /*
     * Whichever side is not exhausted keeps its trailing fields unread: they are present on one
     * side only, which is not a mismatch.
     */
    WT_RET_NOTFOUND_OK(sh_ret);
    WT_RET_NOTFOUND_OK(md_ret);
    return (0);
}

/*
 * __disagg_check_meta_all_fields --
 *     Compare every configuration field between the local and the shared metadata entries.
 */
static int
__disagg_check_meta_all_fields(
  WT_SESSION_IMPL *session, const char *uri, const char *md_value, const char *sh_value)
{
    WT_CONFIG md_parser, sh_parser;
    WT_DECL_RET;
    const char *cfg[2], *md_merge, *sh_merge;

    md_merge = sh_merge = NULL;

    /*
     * The metadata writers do not sort configuration fields, so merge both entries into sorted
     * order at every nesting level before merging them in a single pass.
     */
    cfg[0] = md_value;
    cfg[1] = NULL;
    WT_ERR(__wt_config_merge(session, cfg, NULL, &md_merge));
    cfg[0] = sh_value;
    WT_ERR(__wt_config_merge(session, cfg, NULL, &sh_merge));

    __wt_config_init(session, &sh_parser, sh_merge);
    __wt_config_init(session, &md_parser, md_merge);
    ret = __disagg_check_meta_fields(session, uri, "", &md_parser, &sh_parser);

err:
    __wt_free(session, md_merge);
    __wt_free(session, sh_merge);
    return (ret);
}
#else
/*
 * __disagg_check_meta_id --
 *     Compare the btree id of a file entry between the local and the shared metadata, panicking
 *     when they differ.
 */
static int
__disagg_check_meta_id(
  WT_SESSION_IMPL *session, const char *uri, const char *md_value, const char *sh_value)
{
    WT_CONFIG_ITEM md_cval, sh_cval;

    WT_RET(__wt_config_getones(session, sh_value, "id", &sh_cval));
    WT_RET(__wt_config_getones(session, md_value, "id", &md_cval));

    /* An id mismatch means the checkpoint would be read under the wrong btree identity. */
    if (sh_cval.val != md_cval.val)
        WT_RET_PANIC(session, EINVAL,
          "checkpoint pickup metadata mismatch for \"%s\": the value of \"id\" differs between "
          "the local (\"%.*s\") and the shared (\"%.*s\") metadata",
          uri, (int)md_cval.len, md_cval.str, (int)sh_cval.len, sh_cval.str);
    return (0);
}
#endif /* HAVE_DIAGNOSTIC */

/*
 * __disagg_check_meta_match --
 *     Verify that the immutable configuration fields of a metadata entry agree between the local
 *     and the shared metadata. A divergence means the checkpoint would be interpreted under the
 *     wrong schema or btree identity, silently corrupting reads, so panic instead.
 */
static int
__disagg_check_meta_match(WT_SESSION_IMPL *session, WT_CURSOR *sh_cursor, WT_CURSOR *md_cursor)
{
    const char *md_key, *md_value, *sh_key, *sh_value;

    WT_RET(sh_cursor->get_key(sh_cursor, &sh_key));
    WT_RET(md_cursor->get_key(md_cursor, &md_key));
    WT_ASSERT(session, strcmp(sh_key, md_key) == 0);

    /*
     * Skip system tables with fixed identities: their local entries are created on each node rather
     * than copied from the shared metadata, so their fields can legitimately differ.
     */
    if (WT_IS_URI_METADATA(sh_key) || WT_IS_URI_HS(sh_key))
        return (0);

    /*
     * Non-diagnostic builds validate only the btree id of file entries; diagnostic builds compare
     * every field of every entry.
     */
#ifndef HAVE_DIAGNOSTIC
    if (!WT_PREFIX_MATCH(sh_key, "file:"))
        return (0);
#endif

    WT_RET(sh_cursor->get_value(sh_cursor, &sh_value));
    WT_RET(md_cursor->get_value(md_cursor, &md_value));

    /* Fast path: identical values need no field comparison. */
    if (strcmp(sh_value, md_value) == 0)
        return (0);

#ifdef HAVE_DIAGNOSTIC
    return (__disagg_check_meta_all_fields(session, sh_key, md_value, sh_value));
#else
    return (__disagg_check_meta_id(session, sh_key, md_value, sh_value));
#endif
}

/*
 * The btree IDs of every stable file the local metadata will hold once this pickup has applied the
 * checkpoint, collected as the walk over the local and shared metadata passes each entry.
 */
typedef struct {
    uint32_t *ids;
    size_t allocated;
    size_t count;
} WT_DISAGG_STABLE_BTREE_IDS;

/*
 * __disagg_stable_btree_ids_add --
 *     Record a metadata entry's btree ID, skipping anything that is not the stable constituent of a
 *     layered table.
 */
static int
__disagg_stable_btree_ids_add(WT_SESSION_IMPL *session,
  WT_DISAGG_STABLE_BTREE_IDS *stable_btree_ids, const char *key, const char *value)
{
    WT_CONFIG_ITEM id_val;

    if (!WT_PREFIX_MATCH(key, "file:") || !WT_URI_IS_STABLE(key))
        return (0);

    WT_RET(__wt_config_getones(session, value, "id", &id_val));
    WT_RET(__wt_realloc_def(
      session, &stable_btree_ids->allocated, stable_btree_ids->count + 1, &stable_btree_ids->ids));
    stable_btree_ids->ids[stable_btree_ids->count++] = (uint32_t)id_val.val;

    return (0);
}

/*
 * __disagg_insert_meta --
 *     Copy the current entry from a shared metadata cursor into the local metadata table.
 */
static int
__disagg_insert_meta(
  WT_SESSION_IMPL *session, WT_CURSOR *sh_cursor, WT_DISAGG_STABLE_BTREE_IDS *stable_btree_ids)
{
    WT_DECL_RET;
    const char *key, *value;

    WT_ERR(sh_cursor->get_key(sh_cursor, &key));
    WT_ERR(sh_cursor->get_value(sh_cursor, &value));
    /* A tracked insert, so a failed merge unrolls it. */
    WT_ERR_MSG_CHK(session, __wt_metadata_insert(session, key, value),
      "Failed to insert metadata for key \"%s\"", key);
    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Inserted new key to the local metadata \"%s\": \"%s\"", key, value);
    WT_STAT_CONN_INCR(session, disagg_pick_up_file_meta_inserted);

    /* Track from here so that every path that adopts a leader-assigned ID is covered. */
    WT_ERR(__disagg_stable_btree_ids_add(session, stable_btree_ids, key, value));

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
    WT_DECL_ITEM(old_uri_buf);
    WT_DECL_RET;
    char *cfg_ret, *current_value_copy;
    const char *checkpoint_name, *current_value;
    const char *md_file_key, *metadata_value, *sh_file_key;
    bool discard;

    cfg_ret = current_value_copy = NULL;
    checkpoint_name = NULL;
    discard = false;

    WT_ERR(__wt_scr_alloc(session, 0, &old_uri_buf));
    WT_ERR(sh_file_cursor->get_key(sh_file_cursor, &sh_file_key));
    WT_ERR(sh_file_cursor->get_value(sh_file_cursor, &metadata_value));
    WT_ERR(__wt_config_getones(session, metadata_value, "checkpoint", &cval));

    /* Check that the local metadata cursor is positioned at the same key. */
    WT_ERR(md_file_cursor->get_key(md_file_cursor, &md_file_key));
    WT_ASSERT(session, strcmp(md_file_key, sh_file_key) == 0);

    WT_ERR(md_file_cursor->get_value(md_file_cursor, &current_value));
    /* Copy before further cursor ops; also used as discard-check input. */
    WT_ERR(__wt_strdup(session, current_value, &current_value_copy));
    WT_ERR(__wt_config_getones(session, current_value_copy, "checkpoint", &cval_cur));
    /* Nothing to do if the local checkpoint already matches the shared one. */
    if (__wt_string_slice_cmp(cval_cur.str, cval_cur.len, cval.str, cval.len) == 0)
        goto err;

    WT_ERR(__disagg_replace_checkpoint(session, current_value_copy, &cval, &cfg_ret));

    /* A tracked update, so a failed merge unrolls it. */
    WT_ERR_MSG_CHK(session, __wt_metadata_update(session, sh_file_key, cfg_ret),
      "Failed to update metadata for key \"%s\"", sh_file_key);
    WT_STAT_CONN_INCR(session, disagg_pick_up_file_meta_updated);

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Updated the local metadata for key \"%s\" to include new checkpoint: \"%.*s\"", sh_file_key,
      (int)cval.len, cval.str);

    /*
     * Mark any matching data handles associated with the previous checkpoint to be out of date. Any
     * new opens will get the new metadata.
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
__disagg_apply_checkpoint_meta(WT_SESSION_IMPL *session, const WT_DISAGG_CHECKPOINT_META *ckpt_meta,
  wt_timestamp_t ckpt_schema_epoch, bool is_startup)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR *md_cursors[WT_DISAGG_CURSOR_COUNT], *md_write_cursor,
      *sh_cursors[WT_DISAGG_CURSOR_COUNT];
    WT_DECL_ITEM(current_buf);
    WT_DECL_ITEM(metadata_uri_buf);
    WT_DECL_RET;
    WT_DISAGG_STABLE_BTREE_IDS stable_btree_ids;
    WT_SHARED_METADATA_OP latest_op;
    WT_TIMER apply_timer;
    wt_timestamp_t latest_epoch;
    uint64_t apply_elapsed_ms;
    uint32_t dup_id, existing_tables, new_tables, new_ingest;
    size_t current_len;
    int i;
    char *first_uri, *layered_ingest_uri, *second_uri;
    const char *cfg[2], *metadata_checkpoint_name, *metadata_value;
    const char *md_keys[WT_DISAGG_CURSOR_COUNT], *sh_keys[WT_DISAGG_CURSOR_COUNT];
    const char *current;
    bool md_has[WT_DISAGG_CURSOR_COUNT], sh_has[WT_DISAGG_CURSOR_COUNT], strict;

    for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++)
        md_cursors[i] = sh_cursors[i] = NULL;
    md_write_cursor = NULL;
    WT_CLEAR(stable_btree_ids);

    metadata_checkpoint_name = NULL;
    first_uri = layered_ingest_uri = second_uri = NULL;
    existing_tables = new_tables = new_ingest = 0;

    /* Whether to check that the local and shared metadata contain the same layered tables. */
    strict = F_ISSET(&S2C(session)->disaggregated_storage, WT_DISAGG_STRICT_CHECKPOINT_METADATA);

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

    /*
     * Open private metadata cursors on the local metadata table, leaving the session's cached one
     * free for the tracked metadata updates the merge makes.
     */
    for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++)
        WT_ERR(__wt_metadata_cursor_open(session, NULL, &md_cursors[i]));
    WT_ERR(__wt_metadata_cursor_open(session, NULL, &md_write_cursor));

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

        /*
         * Collect the IDs the local metadata already holds. Applying a checkpoint only ever moves a
         * file's checkpoint forward, never its ID, so these survive the walk unchanged.
         */
        if (md_has[WT_DISAGG_CURSOR_FILE]) {
            WT_ERR(md_cursors[WT_DISAGG_CURSOR_FILE]->get_value(
              md_cursors[WT_DISAGG_CURSOR_FILE], &metadata_value));
            WT_ERR(__disagg_stable_btree_ids_add(
              session, &stable_btree_ids, md_keys[WT_DISAGG_CURSOR_FILE], metadata_value));
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

        /* Verify that the immutable metadata fields agree before adopting the new checkpoint. */
        for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++)
            if (md_has[i] && sh_has[i])
                WT_ERR(__disagg_check_meta_match(session, sh_cursors[i], md_cursors[i]));

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
                  session, sh_cursors[WT_DISAGG_CURSOR_FILE], &stable_btree_ids));
            ++existing_tables;
        } else if (!md_has[WT_DISAGG_CURSOR_LAYERED] && sh_has[WT_DISAGG_CURSOR_LAYERED]) {
            /*
             * The shared metadata has a layered: entry but the local metadata does not. This could
             * be a new layered table that we should pick up, but it could also mean that we have
             * already dropped the table locally and should not recreate it as a result.
             */
            latest_op = __wti_disagg_table_latest_create_remove(session, current, &latest_epoch);
            if (strict &&
              (latest_op != WT_SHARED_METADATA_REMOVE || latest_epoch <= ckpt_schema_epoch))
                WT_ERR_PANIC(session, EINVAL,
                  "strict checkpoint metadata validation failed: table \"%s\" is present in the "
                  "shared metadata but not in the local metadata, and is not explained by a "
                  "pending REMOVE with a schema epoch greater than the checkpoint's schema epoch "
                  "%" PRIu64 "; latest queued operation: %s at schema epoch %" PRIu64,
                  current, ckpt_schema_epoch, __wti_disagg_shared_metadata_op_to_string(latest_op),
                  latest_epoch);
            if (latest_op == WT_SHARED_METADATA_REMOVE)
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
                        session, layered_ingest_uri, metadata_value, is_startup),
                      "Failed to create missing ingest table \"%s\" from \"%s\"",
                      layered_ingest_uri, metadata_value);
                    ++new_ingest;
                }
                __wt_free(session, layered_ingest_uri);
                layered_ingest_uri = NULL;
            }
            WT_ERR(__disagg_insert_meta(
              session, sh_cursors[WT_DISAGG_CURSOR_LAYERED], &stable_btree_ids));
            WT_ERR(
              __disagg_insert_meta(session, sh_cursors[WT_DISAGG_CURSOR_FILE], &stable_btree_ids));
            if (sh_has[WT_DISAGG_CURSOR_COLGROUP])
                WT_ERR(__disagg_insert_meta(
                  session, sh_cursors[WT_DISAGG_CURSOR_COLGROUP], &stable_btree_ids));
            if (sh_has[WT_DISAGG_CURSOR_TABLE])
                WT_ERR(__disagg_insert_meta(
                  session, sh_cursors[WT_DISAGG_CURSOR_TABLE], &stable_btree_ids));
            ++new_tables;
        } else if (md_has[WT_DISAGG_CURSOR_LAYERED] && !sh_has[WT_DISAGG_CURSOR_LAYERED]) {
            /*
             * The local metadata has a layered: entry but the shared metadata does not - a dropped
             * layered table.
             *
             * FIXME-WT-17746: Remove the local metadata entries for the dropped table.
             */
            latest_op = __wti_disagg_table_latest_create_remove(session, current, &latest_epoch);
            if (strict &&
              (latest_op != WT_SHARED_METADATA_CREATE || latest_epoch <= ckpt_schema_epoch))
                WT_ERR_PANIC(session, EINVAL,
                  "strict checkpoint metadata validation failed: table \"%s\" is present in "
                  "the local metadata but not in the shared metadata, and there is no pending "
                  "CREATE with a schema epoch greater than the checkpoint's schema epoch "
                  "%" PRIu64 "; latest queued operation: %s at schema epoch %" PRIu64,
                  current, ckpt_schema_epoch, __wti_disagg_shared_metadata_op_to_string(latest_op),
                  latest_epoch);
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
                  session, sh_cursors[WT_DISAGG_CURSOR_TABLE], &stable_btree_ids));
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
                  session, sh_cursors[WT_DISAGG_CURSOR_COLGROUP], &stable_btree_ids));

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
                  session, sh_cursors[WT_DISAGG_CURSOR_FILE], &stable_btree_ids));
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

    /*
     * A stable ID is the leader's key into shared storage, so a follower can neither renumber the
     * incoming table nor drop the local one it collides with, and no retry can resolve it. Halt
     * before either handle is opened.
     */
    if (__wt_metadata_btree_ids_find_duplicate(
          stable_btree_ids.ids, stable_btree_ids.count, &dup_id)) {
        WT_ERR(__wt_metadata_stable_uris_for_id(session, dup_id, &first_uri, &second_uri));
        WT_ERR_PANIC(session, EINVAL,
          "checkpoint pickup would leave btree ID %" PRIu32
          " shared by \"%s\" and \"%s\" in the local metadata",
          dup_id, first_uri, second_uri);
    }

    /* Fail the merge on the failpoint to exercise the unroll and retry paths. */
    if (__wt_failpoint(
          session, WT_TIMING_STRESS_FAILPOINT_DISAGG_CHECKPOINT_APPLY, 10 * WT_THOUSAND))
        WT_ERR(EBUSY);

    __wt_timer_evaluate_ms(session, &apply_timer, &apply_elapsed_ms);
    WT_STAT_CONN_SET(session, disagg_apply_checkpoint_meta_time, apply_elapsed_ms);
    __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Checkpoint pickup processed %" PRIu32 " existing tables, %" PRIu32 " new tables, %" PRIu32
      " new ingest tables in %" PRIu64 "ms",
      existing_tables, new_tables, new_ingest, apply_elapsed_ms);

done:
err:
    __wt_free(session, stable_btree_ids.ids);
    __wt_free(session, first_uri);
    __wt_free(session, second_uri);
    __wt_free(session, metadata_checkpoint_name);
    __wt_free(session, layered_ingest_uri);
    __wt_scr_free(session, &current_buf);
    __wt_scr_free(session, &metadata_uri_buf);

    if (md_write_cursor != NULL)
        WT_TRET(md_write_cursor->close(md_write_cursor));
    for (i = 0; i < WT_DISAGG_CURSOR_COUNT; i++) {
        if (md_cursors[i] != NULL)
            WT_TRET(md_cursors[i]->close(md_cursors[i]));
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
 * __disagg_defer_checkpoint --
 *     Remember a checkpoint whose adoption is deferred while transactional snapshots that predate
 *     it are active, taking ownership of the metadata copy. The queue has its own lock, so
 *     deliveries never wait behind an adoption.
 */
static int
__disagg_defer_checkpoint(WT_SESSION_IMPL *session, char **meta_strp, uint64_t lsn)
{
    WT_DECL_RET;
    WT_DISAGG_DEFERRED_CKPT *entry, *newest;
    WT_DISAGGREGATED_STORAGE *disagg = &S2C(session)->disaggregated_storage;

    __wt_spin_lock(session, &disagg->deferred_ckpt_lock);

    /* A checkpoint at or behind the newest deferred one carries nothing new. */
    newest = TAILQ_LAST(&disagg->deferred_ckpt_qh, __wt_disagg_deferred_ckpt_qh);
    if (newest != NULL && lsn <= newest->lsn)
        goto err;

    /*
     * Remember every checkpoint not yet adopted, oldest first: a reader only blocks the checkpoints
     * newer than its snapshot, so keeping the intermediate ones lets the node adopt up to the
     * newest checkpoint its readers permit instead of waiting for all of them to finish. Adopting
     * an entry discards all older ones, which bounds the queue.
     */
    WT_ERR(__wt_calloc_one(session, &entry));
    entry->lsn = lsn;
    entry->meta = *meta_strp;
    *meta_strp = NULL;
    TAILQ_INSERT_TAIL(&disagg->deferred_ckpt_qh, entry, q);
    WT_STAT_CONN_INCR(session, disagg_checkpoint_defer);

err:
    __wt_spin_unlock(session, &disagg->deferred_ckpt_lock);
    return (ret);
}

/*
 * __disagg_clear_deferred_checkpoint --
 *     Discard the deferred checkpoints covered by the given LSN.
 */
static void
__disagg_clear_deferred_checkpoint(WT_SESSION_IMPL *session, uint64_t adopted_lsn)
{
    WT_DISAGG_DEFERRED_CKPT *entry;
    WT_DISAGGREGATED_STORAGE *disagg = &S2C(session)->disaggregated_storage;

    __wt_spin_lock(session, &disagg->deferred_ckpt_lock);
    while ((entry = TAILQ_FIRST(&disagg->deferred_ckpt_qh)) != NULL && entry->lsn <= adopted_lsn) {
        TAILQ_REMOVE(&disagg->deferred_ckpt_qh, entry, q);
        __wt_free(session, entry->meta);
        __wt_free(session, entry);
    }
    __wt_spin_unlock(session, &disagg->deferred_ckpt_lock);
}

/*
 * __wti_disagg_clear_deferred_checkpoint_all --
 *     Discard every deferred checkpoint unconditionally, e.g. on a role change: the new role makes
 *     none of them adoptable, regardless of LSN.
 */
void
__wti_disagg_clear_deferred_checkpoint_all(WT_SESSION_IMPL *session)
{
    __disagg_clear_deferred_checkpoint(session, UINT64_MAX);
}

/*
 * __disagg_deferred_ckpt_queued --
 *     Check whether any checkpoint is waiting to be adopted. This is an unsafe check to avoid
 *     claiming the queue lock: only the pickup server asks, to choose how long to sleep, and an
 *     answer stale in either direction only delays a look at the queue. This is in its own function
 *     to suppress the TSan warning.
 */
static WT_INLINE bool
__disagg_deferred_ckpt_queued(WT_SESSION_IMPL *session)
{
    WT_DISAGGREGATED_STORAGE *disagg = &S2C(session)->disaggregated_storage;

    return (!TAILQ_EMPTY(&disagg->deferred_ckpt_qh));
}

/*
 * __disagg_deferred_pickup_run_chk --
 *     Check to decide if the deferred pickup server should continue running.
 */
static WT_INLINE bool
__disagg_deferred_pickup_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_DISAGG_PICKUP));
}

/*
 * __disagg_deferred_pickup_server --
 *     Background server adopting deferred checkpoints once the transactions blocking them end. It
 *     sleeps until a pinning transaction finishes or a checkpoint is deferred.
 */
static WT_THREAD_RET
__disagg_deferred_pickup_server(void *arg)
{
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *disagg;
    WT_SESSION_IMPL *session;
    int last_error;

    session = arg;
    disagg = &S2C(session)->disaggregated_storage;
    last_error = 0;

    for (;;) {
        /*
         * Sleep until signalled: a deferral arms the server and a pin release may unblock an
         * adoption. While anything is queued, wake periodically as a backstop against a wakeup lost
         * to a release racing a delivery; this is not an adoption deadline.
         */
        __wt_cond_wait(session, disagg->deferred_pickup_cond,
          __disagg_deferred_ckpt_queued(session) ? 10 * WT_MILLION : 0,
          __disagg_deferred_pickup_run_chk);

        /*
         * A conflict with a concurrent pickup (EBUSY) clears on its own, and the blockers that
         * would produce the next signal may already be gone, so spin here rather than returning to
         * an indefinite wait that could strand the queue. Back off so the conflict is not hot.
         *
         * Any other failure may well be permanent, so report it once and go back to waiting: the
         * queue stays armed and the backstop retries it, without a warning per retry.
         */
        while (__disagg_deferred_pickup_run_chk(session)) {
            ret = __wti_disagg_deferred_pickup_retry(session, false);
            if (ret != EBUSY)
                break;

            __wt_sleep(0, WT_DISAGG_RETRY_SLEEP_USECS);
        }
        if (ret != last_error) {
            last_error = ret;
            if (ret != 0)
                __wt_verbose_warning(session, WT_VERB_DISAGGREGATED_STORAGE,
                  "deferred checkpoint pickup failed: %s", __wt_strerror(session, ret, NULL, 0));
        }

        if (!__disagg_deferred_pickup_run_chk(session))
            break;
    }

    return (WT_THREAD_RET_VALUE);
}

/*
 * __wt_disagg_deferred_pickup_signal --
 *     Wake the deferred pickup server: called when a pinning transaction finishes and when a
 *     checkpoint is deferred. A released pin generation is passed so releases that cannot unblock
 *     any deferred checkpoint skip the wakeup; zero signals unconditionally.
 */
void
__wt_disagg_deferred_pickup_signal(WT_SESSION_IMPL *session, uint64_t released_gen)
{
    WT_DISAGGREGATED_STORAGE *disagg = &S2C(session)->disaggregated_storage;

    /*
     * Every deferred checkpoint was delivered, so a pin taken at or after the newest delivered
     * checkpoint was blocking none of them. This needs no state of its own and skips the wakeup for
     * the common case: a reader that began after the last delivery, which is every reader once
     * nothing is deferred.
     */
    if (released_gen != 0 && released_gen >= __wt_gen(session, WT_GEN_DISAGG_CKPT))
        return;
    if (disagg->deferred_pickup_cond != NULL)
        __wt_cond_signal(session, disagg->deferred_pickup_cond);
}

/*
 * __wti_disagg_deferred_pickup_server_create --
 *     Start the deferred checkpoint pickup server.
 */
int
__wti_disagg_deferred_pickup_server_create(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn = S2C(session);
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *disagg = &conn->disaggregated_storage;

    if (disagg->deferred_pickup_session != NULL)
        return (0);

    FLD_SET(conn->server_flags, WT_CONN_SERVER_DISAGG_PICKUP);

    WT_ERR(__wt_open_internal_session(
      conn, "disagg-pickup-server", false, 0, 0, &disagg->deferred_pickup_session));
    /* The condition variable survives a server stop: see the comment in the destroy function. */
    if (disagg->deferred_pickup_cond == NULL)
        WT_ERR(__wt_cond_alloc(disagg->deferred_pickup_session, "disagg deferred pickup",
          &disagg->deferred_pickup_cond));
    WT_ERR(__wt_thread_create(disagg->deferred_pickup_session, &disagg->deferred_pickup_tid,
      __disagg_deferred_pickup_server, disagg->deferred_pickup_session));
    disagg->deferred_pickup_tid_set = true;
    return (0);

err:
    WT_TRET(__wti_disagg_deferred_pickup_server_destroy(session));
    return (ret);
}

/*
 * __wti_disagg_deferred_pickup_server_destroy --
 *     Stop the deferred checkpoint pickup server.
 */
int
__wti_disagg_deferred_pickup_server_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn = S2C(session);
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *disagg = &conn->disaggregated_storage;

    FLD_CLR(conn->server_flags, WT_CONN_SERVER_DISAGG_PICKUP);
    if (disagg->deferred_pickup_tid_set) {
        __wt_cond_signal(session, disagg->deferred_pickup_cond);
        WT_TRET(__wt_thread_join(session, &disagg->deferred_pickup_tid));
        disagg->deferred_pickup_tid_set = false;
    }
    /*
     * Do not destroy the condition variable: a step-up stops the server on a live system, and any
     * session releasing a pinned snapshot may be signaling it concurrently. Signaling with no
     * waiter is harmless, so the condition variable lives until the connection tears down
     * disaggregated storage single-threaded.
     */
    if (disagg->deferred_pickup_session != NULL) {
        WT_TRET(__wt_session_close_internal(disagg->deferred_pickup_session));
        disagg->deferred_pickup_session = NULL;
    }
    return (ret);
}

/*
 * __disagg_deferred_copy --
 *     Copy out the deferred checkpoint to adopt: the newest one outright when forced (a step-up
 *     must continue from the newest checkpoint), otherwise the newest one no active snapshot
 *     predates. The walk relies on ordering along the oldest-first queue: a snapshot predating a
 *     checkpoint predates every newer one, so it stops at the first blocked entry, having
 *     remembered the newest one that passed. The queue has its own lock, so the copy never waits
 *     behind an adoption. Returns WT_NOTFOUND when no entry may be adopted.
 */
static int
__disagg_deferred_copy(WT_SESSION_IMPL *session, bool force, char **metap, uint64_t *lsnp)
{
    WT_DECL_RET;
    WT_DISAGG_DEFERRED_CKPT *entry, *selected;
    WT_DISAGGREGATED_STORAGE *disagg = &S2C(session)->disaggregated_storage;
    uint64_t oldest_gen;

    *metap = NULL;
    *lsnp = WT_DISAGG_LSN_NONE;

    /*
     * The oldest pin decides the whole queue, so scan the sessions once, before taking the queue
     * lock. A snapshot established after the scan pins a generation newer than every delivered
     * checkpoint, so it cannot be one this selection needed to see.
     */
    oldest_gen = force ? 0 : __wt_gen_oldest(session, WT_GEN_DISAGG_CKPT);

    __wt_spin_lock(session, &disagg->deferred_ckpt_lock);
    selected = NULL;
    TAILQ_FOREACH (entry, &disagg->deferred_ckpt_qh, q) {
        /* A pin that does not cover the LSN means a snapshot predates it. */
        if (!force && oldest_gen <= entry->lsn)
            break;
        selected = entry;
    }
    if (selected == NULL)
        ret = WT_NOTFOUND;
    else {
        *lsnp = selected->lsn;
        /*
         * Copy the metadata: the adoption runs without this lock, and a concurrent adoption's
         * pruning may free the entry meanwhile.
         */
        ret = __wt_strdup(session, selected->meta, metap);
    }
    __wt_spin_unlock(session, &disagg->deferred_ckpt_lock);
    return (ret);
}

/*
 * __wti_disagg_deferred_pickup_retry --
 *     Retry adopting a checkpoint whose pickup was deferred for active snapshots. Called
 *     periodically so a deferred checkpoint is adopted once the snapshots that blocked it end.
 */
int
__wti_disagg_deferred_pickup_retry(WT_SESSION_IMPL *session, bool force)
{
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *disagg = &S2C(session)->disaggregated_storage;
    uint64_t deferred_lsn;
    char *meta_copy = NULL;

    deferred_lsn = WT_DISAGG_LSN_NONE;

    /* Having nothing to adopt is the common case, not a failure. */
    ret = __disagg_deferred_copy(session, force, &meta_copy, &deferred_lsn);
    if (ret == WT_NOTFOUND)
        return (0);
    WT_RET(ret);

    ret = __disagg_adopt_deferred_checkpoint_meta(session, meta_copy, strlen(meta_copy));

    /*
     * A concurrent pickup may have adopted this checkpoint, or a newer one, between the copy above
     * and the adoption, which leaves the deferred pickup satisfied. That race has two outcomes: the
     * adoption rejects the now-older checkpoint (EINVAL), or its metadata merge conflicts with the
     * winner and unrolls (EBUSY). Anything else is a real failure, and a panic in particular must
     * not be swallowed here just because the winner published its LSN first.
     */
    if ((ret == EINVAL || ret == EBUSY) &&
      __wt_atomic_load_uint64_acquire(&disagg->last_checkpoint_meta_lsn) >= deferred_lsn) {
        /*
         * Prune the superseded entries: the copy above is not atomic with adoptions, so an entry
         * covered by a concurrent adoption may linger and would otherwise be selected again.
         */
        __disagg_clear_deferred_checkpoint(
          session, __wt_atomic_load_uint64_acquire(&disagg->last_checkpoint_meta_lsn));
        ret = 0;
    }

    __wt_free(session, meta_copy);
    return (ret);
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
    /* Publish the adopted LSN as a statistic: adoption is asynchronous when deferred. */
    WT_STAT_CONN_SET(session, disagg_checkpoint_meta_lsn, (int64_t)ckpt_meta->metadata_lsn);

    /* The adoption satisfies any pending deferred pickup this checkpoint covers. */
    __disagg_clear_deferred_checkpoint(session, ckpt_meta->metadata_lsn);

    /* Update the timestamps. */
    __wt_atomic_store_uint64_release(
      &conn->disaggregated_storage.last_checkpoint_schema_epoch, metadata->schema_epoch);
    __wt_atomic_store_uint64_release(
      &conn->disaggregated_storage.last_checkpoint_timestamp, metadata->checkpoint_timestamp);
    __wt_atomic_store_uint64_release(
      &conn->disaggregated_storage.last_checkpoint_oldest_timestamp, metadata->oldest_timestamp);
    conn->txn_global.last_ckpt_disaggregated_schema_epoch = metadata->schema_epoch;
    /* Release store to pair with the acquire load in sweep. */
    __wt_atomic_store_uint64_release(
      &conn->txn_global.last_ckpt_timestamp, metadata->checkpoint_timestamp);

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
 * __disagg_adopt_checkpoint_meta --
 *     Merge the checkpoint's metadata into the local metadata as one tracked unit: on failure the
 *     tracking unrolls every update already made, including any ingest tables created along the
 *     way, so the merge either completes or leaves no trace.
 */
static int
__disagg_adopt_checkpoint_meta(WT_SESSION_IMPL *session, const WT_DISAGG_CHECKPOINT_META *ckpt_meta,
  const WT_DISAGG_METADATA *metadata, bool is_startup)
{
    WT_DECL_RET;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->schema_lock);

    WT_RET(__wt_meta_track_on(session));

    /* Update our local metadata with the new checkpoint entry. */
    WT_ERR(__disagg_save_checkpoint_meta_local(session, metadata));

    /* Apply the metadata for the other tables from the shared metadata table. */
    WT_ERR(__disagg_apply_checkpoint_meta(session, ckpt_meta, metadata->schema_epoch, is_startup));

err:
    WT_TRET(__wt_meta_track_off(session, true, ret != 0));
    return (ret);
}

/*
 * __disagg_pick_up_checkpoint --
 *     Pick up a new checkpoint. A caller that raced another adoption expects to find the checkpoint
 *     superseded and says so, which reports that outcome quietly rather than as an error.
 */
static int
__disagg_pick_up_checkpoint(
  WT_SESSION_IMPL *session, const WT_DISAGG_CHECKPOINT_META *ckpt_meta, bool superseded_ok)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGG_METADATA metadata;
    WT_ITEM metadata_buf;
    WT_TIMER pickup_timer;
    uint64_t current_meta_lsn, pickup_elapsed_ms;
    char ts_string[3][WT_TS_INT_STRING_SIZE];
    bool is_startup;

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
    if (ckpt_meta->metadata_lsn < current_meta_lsn) {
        if (superseded_ok) {
            __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Skipping a superseded checkpoint: current metadata LSN = %" PRIu64
              ", new metadata LSN = %" PRIu64,
              current_meta_lsn, ckpt_meta->metadata_lsn);
            return (EINVAL);
        }
        WT_RET_MSG(session, EINVAL,
          "Attempting to pick up an older checkpoint: current metadata LSN = %" PRIu64
          ", new metadata LSN = %" PRIu64,
          current_meta_lsn, ckpt_meta->metadata_lsn);
    }
    is_startup = current_meta_lsn == WT_DISAGG_LSN_NONE;

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
     * Refresh the pinned timestamp before checking it against the checkpoint's oldest timestamp.
     * The cached value may lag behind the actual minimum held by active transactions; using a stale
     * (lower) value here would cause a false panic below.
     */
    __wt_txn_update_pinned_timestamp(session, false);
    uint64_t pinned_timestamp;
    __wt_txn_pinned_timestamp(session, &pinned_timestamp);
    if (pinned_timestamp != WT_TS_NONE && metadata.oldest_timestamp > pinned_timestamp) {
        WT_TRET(__wt_verbose_dump_sessions(session, false));
        WT_IGNORE_RET(__wt_panic(session, EINVAL,
          "Disaggregated storage checkpoint oldest_timestamp %s is greater than the current pinned "
          "timestamp %s",
          __wt_timestamp_to_string(metadata.oldest_timestamp, ts_string[0]),
          __wt_timestamp_to_string(pinned_timestamp, ts_string[1])));
    }

    /*
     * Part 1: Get the metadata of the shared metadata table.
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

    /*
     * Adopt the high-water mark of write generations the checkpoint's writer (the leader) recorded,
     * so that if this node later steps up its base write generation already sits past the former
     * leader's generations and a checkpoint it then reopens is recognized as belonging to an
     * earlier generation range. A follower reads foreign checkpoints correctly regardless of this
     * value because it resets their ids on open by role; a checkpoint written before the high-water
     * mark was recorded (an upgrade) carries no aggregate to adopt, so remember that a node
     * becoming leader must derive the base write generation from its local metadata instead.
     *
     * Concurrency: we hold the checkpoint lock, the only writer of the base write generation once
     * followers are active; the base write generation is monotonic.
     */
    if (metadata.max_write_gen != 0) {
        WT_ASSERT_ALWAYS(session,
          __wt_atomic_load_uint64_relaxed(&conn->base_write_gen) <=
            __wt_atomic_load_uint64_relaxed(&conn->max_write_gen),
          "base_write_gen exceeds max_write_gen");
        __wt_atomic_store_uint64_relaxed(&conn->base_write_gen,
          WT_MAX(
            __wt_atomic_load_uint64_relaxed(&conn->base_write_gen), metadata.max_write_gen + 1));
        __wt_atomic_store_uint64_relaxed(&conn->max_write_gen,
          WT_MAX(__wt_atomic_load_uint64_relaxed(&conn->max_write_gen),
            __wt_atomic_load_uint64_relaxed(&conn->base_write_gen)));
        conn->disaggregated_storage.base_write_gen_missing = false;
    } else
        conn->disaggregated_storage.base_write_gen_missing = true;

    /* Load crypt key data with the key provider extension, if any. */
    WT_ERR(__wti_disagg_load_crypt_key(session, &metadata));

    /*
     * Part 2: Merge the checkpoint's metadata into the local metadata. The merge runs under
     * metadata tracking, so a failure unrolls the updates already made and leaves the node on its
     * previous checkpoint, retryable; a crash mid-merge relies on the node discarding its local
     * state on restart. Data handles marked outdated along the way stay marked across an unroll,
     * which only costs reopening them.
     */
    WT_WITH_SCHEMA_LOCK(
      session, ret = __disagg_adopt_checkpoint_meta(session, ckpt_meta, &metadata, is_startup));
    WT_ERR(ret);

    /*
     * A no-epoch checkpoint clears the whole queue. An epoch-world node never picks up a no-epoch
     * checkpoint, so its live stable epoch is unset here and the queue holds no published entries
     * to lose.
     */
    WT_ASSERT(session,
      metadata.schema_epoch != WT_SCHEMA_EPOCH_NONE ||
        __wt_get_stable_disaggregated_schema_epoch(session) == WT_SCHEMA_EPOCH_NONE);

    /*
     * Part 3: Do the bookkeeping.
     */
    __wti_disagg_shared_metadata_queue_prune(session, metadata.schema_epoch);

    /*
     * The merge is complete: a failure from here leaves the local metadata resolving to the new
     * checkpoint while the published LSN still admits only the old one, and there is no
     * compensation short of completing the adoption, so it is fatal.
     */
    if ((ret = __disagg_finalize_checkpoint_meta(session, ckpt_meta, &metadata)) != 0)
        WT_ERR_PANIC(
          session, ret, "failed to adopt a checkpoint after completing its metadata merge");

    /* Log the completion of the checkpoint pick-up. */
    __wt_timer_evaluate_ms(session, &pickup_timer, &pickup_elapsed_ms);
    WT_STAT_CONN_SET(session, disagg_pick_up_checkpoint_time, pickup_elapsed_ms);
    __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Finished picking up disaggregated storage checkpoint: metadata_lsn=%" PRIu64 " in %" PRIu64
      "ms",
      ckpt_meta->metadata_lsn, pickup_elapsed_ms);

err:
    /* A write conflict unrolled the merge cleanly; report it as retryable. */
    if (ret == WT_ROLLBACK)
        ret = EBUSY;

    if (ret == 0) {
        WT_STAT_CONN_INCR(session, layered_table_manager_checkpoints_disagg_pick_up_succeed);
        if (!__wt_atomic_load_bool_relaxed(&conn->layered_table_manager.leader))
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
 * __disagg_pick_up_checkpoint_meta --
 *     Pick up a new checkpoint from metadata config, shared by the loud and the racing callers.
 */
static int
__disagg_pick_up_checkpoint_meta(WT_SESSION_IMPL *session, const char *meta_data,
  size_t meta_data_size, bool force, bool superseded_ok)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *disagg;
    WT_DISAGG_CHECKPOINT_META ckpt_meta;
    WT_SESSION_IMPL *internal_session;
    uint64_t metadata_checksum, pending_lsn;
    char *meta_str;
    bool encoding, prev_adopted, prev_encoding;

    WT_CLEAR(ckpt_meta);
    disagg = &S2C(session)->disaggregated_storage;
    meta_str = NULL;
    internal_session = NULL;
    prev_encoding = F_ISSET(disagg, WT_DISAGG_STABLE_TOMBSTONE_ENCODING);
    prev_adopted = disagg->stable_tombstone_encoding_adopted;

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

    /*
     * Derive the stable tombstone encoding mode from the checkpoint's compatible version and adopt
     * it: in automatic mode the data on disk decides how stable values are decoded. A checkpoint
     * predating the unescaped format (including one predating the version fields entirely) carries
     * escaped stable values. A break-glass-forced mode wins over the derivation: the adopt call
     * keeps the forced mode and warns when the checkpoint disagrees.
     *
     * FIXME-WT-18206: once no supported checkpoint carries escaped stable values, drop the
     * derivation and the escaped mode.
     */
    encoding = ckpt_meta.compatible_version < WT_DISAGG_CHECKPOINT_META_VERSION_STABLE_UNENCODED;
    WT_ERR(__wti_disagg_adopt_stable_tombstone_encoding(session, encoding,
      encoding ? "a checkpoint compatible version predating the unescaped format" :
                 "the checkpoint compatible version"));

    /*
     * Publish the incoming checkpoint before doing anything else: a snapshot established from here
     * on may pin it even though the adoption has not completed, because the arrival of checkpoint
     * metadata implies the content is already replayed into the ingest tables. This keeps such
     * snapshots from being refused at their first stable open once the adoption completes, and from
     * blocking a deferred adoption. Only ever move it forward.
     */
    disagg = &S2C(session)->disaggregated_storage;
    for (;;) {
        pending_lsn = __wt_atomic_load_uint64_acquire(&disagg->pending_checkpoint_meta_lsn);
        if (ckpt_meta.metadata_lsn <= pending_lsn ||
          __wt_atomic_cas_uint64(
            &disagg->pending_checkpoint_meta_lsn, pending_lsn, ckpt_meta.metadata_lsn))
            break;
    }
    /* Advance the checkpoint generation snapshots pin. */
    __wt_gen_advance(session, WT_GEN_DISAGG_CKPT, WT_DISAGG_CKPT_GEN(ckpt_meta.metadata_lsn));
    /* Publish the delivered LSN as a statistic; the adopted LSN is published separately. */
    WT_STAT_CONN_SET(session, disagg_checkpoint_delivered_lsn,
      (int64_t)__wt_atomic_load_uint64_relaxed(&disagg->pending_checkpoint_meta_lsn));

    /*
     * Defer adopting a newer checkpoint while transactional snapshots that predate it are active,
     * so those readers keep opening stable cursors instead of being refused. Forced pickups
     * (startup, step-up) are never deferred.
     */
    if (!force &&
      ckpt_meta.metadata_lsn > __wt_atomic_load_uint64_acquire(&disagg->last_checkpoint_meta_lsn) &&
      __wt_gen_active(session, WT_GEN_DISAGG_CKPT, ckpt_meta.metadata_lsn)) {
        WT_ERR(__disagg_defer_checkpoint(session, &meta_str, ckpt_meta.metadata_lsn));
        /* Wake the pickup server: the delivery may already be adoptable. */
        __wt_disagg_deferred_pickup_signal(session, 0);
        goto err;
    }

    WT_ERR(__wt_open_internal_session(
      S2C(session), "checkpoint-pick-up", false, 0, 0, &internal_session));
    /* Now actually pick up the checkpoint. */
    WT_WITH_CHECKPOINT_LOCK(internal_session,
      ret = __disagg_pick_up_checkpoint(internal_session, &ckpt_meta, superseded_ok));
    WT_ERR(ret);

    /* Record the picked-up checkpoint's version fields; a failed pickup leaves them unchanged. */
    WT_STAT_CONN_SET(session, disagg_checkpoint_storage_version, ckpt_meta.version);
    WT_STAT_CONN_SET(
      session, disagg_checkpoint_storage_compatible_version, ckpt_meta.compatible_version);

err:
    /* A failed pickup must not leave the failed checkpoint's encoding mode adopted. */
    if (ret != 0) {
        if (prev_encoding)
            F_SET(disagg, WT_DISAGG_STABLE_TOMBSTONE_ENCODING);
        else
            F_CLR(disagg, WT_DISAGG_STABLE_TOMBSTONE_ENCODING);
        disagg->stable_tombstone_encoding_adopted = prev_adopted;
        WT_STAT_CONN_SET(session, disagg_stable_tombstone_encoding,
          prev_adopted || F_ISSET(disagg, WT_DISAGG_STABLE_TOMBSTONE_ENCODING_FORCED) ?
            (prev_encoding ? 1 : 2) :
            0);
    }
    if (internal_session != NULL)
        WT_TRET(__wt_session_close_internal(internal_session));
    __wt_free(session, meta_str);
    return (ret);
}

/*
 * __wti_disagg_pick_up_checkpoint_meta --
 *     Pick up a new checkpoint from metadata config. A checkpoint older than the adopted one is an
 *     error here: nothing races this caller, so going backwards means the checkpoint is wrong.
 */
int
__wti_disagg_pick_up_checkpoint_meta(
  WT_SESSION_IMPL *session, const char *meta_data, size_t meta_data_size, bool force)
{
    return (__disagg_pick_up_checkpoint_meta(session, meta_data, meta_data_size, force, false));
}

/*
 * __disagg_adopt_deferred_checkpoint_meta --
 *     Adopt a checkpoint whose pickup was deferred. Finding it superseded is the expected outcome
 *     of losing a race with a concurrent adoption, so it is reported without an error message; the
 *     caller decides whether the race left the deferred pickup satisfied. The deferral check is
 *     skipped: the selection already decided this checkpoint may be adopted.
 */
static int
__disagg_adopt_deferred_checkpoint_meta(
  WT_SESSION_IMPL *session, const char *meta_data, size_t meta_data_size)
{
    return (__disagg_pick_up_checkpoint_meta(session, meta_data, meta_data_size, true, true));
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

/*
 * __ut_disagg_replace_checkpoint --
 *     Unit test wrapper for __disagg_replace_checkpoint.
 */
int
__ut_disagg_replace_checkpoint(
  WT_SESSION_IMPL *session, const char *base, const WT_CONFIG_ITEM *new_ckpt, char **config_ret)
{
    return (__disagg_replace_checkpoint(session, base, new_ckpt, config_ret));
}
#endif
