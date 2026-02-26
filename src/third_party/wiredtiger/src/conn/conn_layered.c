/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * WT_DISAGG_CHECKPOINT_META --
 *     Checkpoint metadata structure for disaggregated storage.
 */
typedef struct __wt_disagg_checkpoint_meta {
    uint64_t metadata_lsn; /* The LSN of the metadata page. */

    bool has_metadata_checksum; /* Whether the metadata page checksum is present. */
    uint32_t metadata_checksum; /* The checksum of the metadata page. */

    uint64_t database_size; /* The total database size. */
    bool has_database_size; /* Whether the database size is present. */
    uint32_t version;       /* The version of the checkpoint_meta. */
    uint32_t
      compatible_version; /* The minimum version of the reader that can use this checkpoint_meta. */
} WT_DISAGG_CHECKPOINT_META;

/* Function prototypes for disaggregated storage and layered tables. */
static void __disagg_set_crypt_header(WT_SESSION_IMPL *session, WT_CRYPT_KEYS *crypt);
static void __disagg_get_crypt_header(WT_ITEM *key_item, WT_CRYPT_HEADER **header);

/*
 * __layered_get_disagg_checkpoint --
 *     Get existing checkpoint information from disaggregated storage.
 */
static int
__layered_get_disagg_checkpoint(WT_SESSION_IMPL *session, const char **cfg,
  uint64_t *complete_checkpoint_lsn, uint64_t *complete_checkpoint_timestamp,
  WT_ITEM *complete_checkpoint_metadata)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_PAGE_LOG *page_log = NULL;
    char *page_log_name;

    conn = S2C(session);
    page_log_name = NULL;

    /*
     * We need our own copy of the page log config string, it must be NULL terminated to look it up.
     */
    WT_ERR(__wt_config_gets(session, cfg, "disaggregated.page_log", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &page_log_name));
    WT_ERR(conn->iface.get_page_log(&conn->iface, page_log_name, &page_log));

    /*
     * Getting the last opened checkpoint and the complete checkpoint from disaggregated storage are
     * only supported in test implementations of the page log interface. This function will never be
     * called in production.
     */
    if (page_log->pl_get_complete_checkpoint_ext == NULL)
        WT_ERR(ENOTSUP);

    ret = page_log->pl_get_complete_checkpoint_ext(page_log, &session->iface,
      complete_checkpoint_lsn, NULL, complete_checkpoint_timestamp, complete_checkpoint_metadata);
    WT_ERR_NOTFOUND_OK(ret, true);

err:
    if (page_log != NULL)
        WT_TRET(page_log->terminate(page_log, &session->iface)); /* dereference */
    __wt_free(session, page_log_name);
    return (ret);
}

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
 * __layered_create_missing_stable_table --
 *     Create a missing stable table from an existing layered table configuration.
 */
static int
__layered_create_missing_stable_table(
  WT_SESSION_IMPL *session, const char *uri, const char *layered_cfg)
{
    WT_DECL_RET;
    const char *constituent_cfg;
    const char *stable_cfg[4] = {WT_CONFIG_BASE(session, table_meta), layered_cfg, NULL, NULL};

    constituent_cfg = NULL;

    /* Disable logging on the stable table so we have timestamps. */
    stable_cfg[2] = "log=(enabled=false)";

    WT_ERR(__wt_config_merge(session, stable_cfg, NULL, &constituent_cfg));
    WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_create(session, uri, constituent_cfg));

err:
    __wt_free(session, constituent_cfg);
    return (ret);
}

/*
 * __layered_create_missing_stable_tables_helper --
 *     Create missing stable tables.
 */
static int
__layered_create_missing_stable_tables_helper(WT_SESSION_IMPL *session)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR *cursor_check, *cursor_scan;
    WT_DECL_RET;
    char *stable_uri;
    const char *layered_uri, *layered_cfg;

    cursor_check = cursor_scan = NULL;
    stable_uri = NULL;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->schema_lock);

    WT_ERR(__wt_metadata_cursor(session, &cursor_check));
    WT_ERR(__wt_metadata_cursor(session, &cursor_scan));

    cursor_scan->set_key(cursor_scan, "layered:");
    WT_ERR(cursor_scan->bound(cursor_scan, "bound=lower"));
    while ((ret = cursor_scan->next(cursor_scan)) == 0) {
        WT_ERR(cursor_scan->get_key(cursor_scan, &layered_uri));
        WT_ERR(cursor_scan->get_value(cursor_scan, &layered_cfg));
        if (!WT_PREFIX_MATCH(layered_uri, "layered:"))
            break;

        /* Extract the stable URI. */
        WT_ERR(__wt_config_getones(session, layered_cfg, "stable", &cval));
        WT_ERR(__wt_strndup(session, cval.str, cval.len, &stable_uri));

        /* Check if the URI exists. */
        cursor_check->set_key(cursor_check, stable_uri);
        WT_ERR_NOTFOUND_OK(cursor_check->search(cursor_check), true);

        /* Create the stable table if it does not exist. */
        if (ret == WT_NOTFOUND) {
            WT_ERR_MSG_CHK(session,
              __layered_create_missing_stable_table(session, stable_uri, layered_cfg),
              "Failed to create missing stable table \"%s\" from \"%s\"", stable_uri, layered_cfg);
            /* Ensure that we properly handle empty tables. */
            WT_ERR(__wt_disagg_enqueue_metadata_operation(
              session, stable_uri, layered_uri + strlen("layered:"), WT_SHARED_METADATA_UPDATE));
            __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Created missing stable table \"%s\" from \"%s\"", stable_uri, layered_uri);
        }

        __wt_free(session, stable_uri);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    __wt_free(session, stable_uri);
    WT_TRET(__wt_metadata_cursor_release(session, &cursor_check));
    WT_TRET(__wt_metadata_cursor_release(session, &cursor_scan));
    return (ret);
}

/*
 * __layered_create_missing_stable_tables --
 *     Create missing stable tables.
 */
static int
__layered_create_missing_stable_tables(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    WT_WITH_SCHEMA_LOCK(session, ret = __layered_create_missing_stable_tables_helper(session));
    return (ret);
}

/*
 * __disagg_get_page --
 *     Read a page from disaggregated storage. Note: The caller assumes ownership of the returned
 *     item.
 */
static int
__disagg_get_page(WT_SESSION_IMPL *session, WT_PAGE_LOG_HANDLE *page_log, uint64_t page_id,
  uint64_t lsn, WT_ITEM *item)
{
    WT_PAGE_LOG_GET_ARGS get_args;
    u_int count, retry;

    if (page_log == NULL)
        return (ENOTSUP);

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->checkpoint_lock);

    WT_CLEAR(get_args);
    get_args.lsn = lsn;

    retry = 0;
    for (;;) {
        count = 1;
        WT_RET(page_log->plh_get(page_log, &session->iface, page_id, 0, &get_args, item, &count));
        WT_ASSERT(session, count <= 1); /* Corrupt data. */

        /* Found the data. */
        if (count == 1)
            break;

        /* Otherwise retry up to 100 times to account for page materialization delay. */
        if (retry > 100) {
            __wt_verbose_error(session, WT_VERB_READ,
              "read failed for page ID %" PRIu64 ", lsn %" PRIu64, page_id, lsn);
            return (EIO);
        }
        __wt_verbose_notice(session, WT_VERB_READ,
          "retry #%" PRIu32 " for page_id %" PRIu64 ", lsn %" PRIu64, retry, page_id, lsn);
        __wt_sleep(0, 10000 + retry * 5000);
        ++retry;
    }

    return (0);
}

/*
 * __disagg_get_meta --
 *     Read metadata from disaggregated storage. Note: The caller assumes ownership of the returned
 *     item.
 */
static int
__disagg_get_meta(WT_SESSION_IMPL *session, uint64_t page_id, uint64_t lsn, WT_ITEM *item)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGGREGATED_STORAGE *disagg;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    WT_ASSERT_ALWAYS(session, page_id <= WT_DISAGG_METADATA_MAX_PAGE_ID,
      "Metadata page ID %" PRIu64 " out of range", page_id);

    WT_RET(__disagg_get_page(session, disagg->page_log_meta, page_id, lsn, item));

    disagg->last_metadata_page_lsn[page_id] = lsn;

    return (0);
}

/*
 * __disagg_get_crypt_key --
 *     Read encryption key data from disaggregated storage. Note: The caller assumes ownership of
 *     the returned item.
 */
static int
__disagg_get_crypt_key(WT_SESSION_IMPL *session, uint64_t page_id, uint64_t lsn, WT_ITEM *item)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGGREGATED_STORAGE *disagg;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    WT_ASSERT_ALWAYS(session, page_id <= WT_DISAGG_KEY_PROVIDER_MAX_PAGE_ID,
      "Multiple key provider pages is not currently supported");

    WT_RET(__disagg_get_page(session, disagg->page_log_key_provider, page_id, lsn, item));

    disagg->last_key_provider_page_lsn[page_id] = lsn;

    return (0);
}

/*
 * __disagg_get_crypt_header --
 *     Copy and byte-swap the crypt header from the key item. Note: This function is not idempotent.
 */
static void
__disagg_get_crypt_header(WT_ITEM *key_item, WT_CRYPT_HEADER **header)
{
    *header = (WT_CRYPT_HEADER *)key_item->data;
}

/*
 * __disagg_validate_crypt --
 *     Validate the crypt header and payload stored in key_item.
 */
static int
__disagg_validate_crypt(WT_SESSION_IMPL *session, WT_ITEM *key_item, WT_CRYPT_HEADER **hdrp)
{
    WT_CRYPT_HEADER *header;
    WT_DECL_RET;
    uint32_t checksum = 0, expected_checksum = 0;

    if (key_item->size < sizeof(WT_CRYPT_HEADER))
        WT_ERR_MSG(session, EIO,
          "Encryption key data too small: expected at least %" WT_SIZET_FMT ", got %" WT_SIZET_FMT,
          sizeof(WT_CRYPT_HEADER), key_item->size);
    __disagg_get_crypt_header(key_item, &header);

    expected_checksum = header->checksum;
#ifdef WORDS_BIGENDIAN
    expected_checksum = __wt_bswap32(expected_checksum);
#endif
    header->checksum = 0;
    checksum = __wt_checksum((uint8_t *)key_item->data, key_item->size);
    if (checksum != expected_checksum)
        WT_ERR_MSG(session, EIO,
          "Encryption key data checksum mismatch: expected %" PRIx32 ", got %" PRIx32,
          expected_checksum, checksum);
    __wt_crypt_header_byteswap(header);

    if (header->header_size < sizeof(WT_CRYPT_HEADER))
        WT_ERR_MSG(session, EIO,
          "Encryption key header is too small: expected at least %" WT_SIZET_FMT ", got %" PRIu8,
          sizeof(WT_CRYPT_HEADER), header->header_size);

    if (key_item->size - header->header_size != header->crypt_size)
        WT_ERR_MSG(session, EIO, "Encryption key data size mismatch: expected %u, got %u",
          header->crypt_size, (uint32_t)(key_item->size - header->header_size));

    /* Check for compatibility versions before validating header fields. */
    if (header->compatible_version > WT_CRYPT_HEADER_COMPATIBLE_VERSION)
        WT_ERR_MSG(session, ENOTSUP,
          "Unsupported encryption key data version %" PRIu8 ", min %" PRIu8, header->version,
          header->compatible_version);

    WT_ASSERT_ALWAYS(session, header->signature == WT_CRYPT_HEADER_SIGNATURE,
      "Invalid encryption key data signature: expected 0x%08" PRIx32 ", got 0x%08" PRIx32,
      WT_CRYPT_HEADER_SIGNATURE, header->signature);

    *hdrp = header;
err:
    return (ret);
}

/*
 * __disagg_put_page --
 *     Write a page to disaggregated storage. This is intended for pages that are not part of a
 *     btree, such as shared turtle files and encryption key.
 */
static int
__disagg_put_page(WT_SESSION_IMPL *session, WT_PAGE_LOG_HANDLE *page_log, uint64_t page_id,
  const WT_ITEM *item, uint64_t last_page_lsn[], uint64_t *lsnp)
{
    WT_PAGE_LOG_PUT_ARGS put_args;

    if (page_log == NULL)
        return (ENOTSUP);

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->checkpoint_lock);

    WT_CLEAR(put_args);

    put_args.backlink_lsn = last_page_lsn[page_id];

    WT_RET(page_log->plh_put(page_log, &session->iface, page_id, 0, &put_args, item));
    last_page_lsn[page_id] = put_args.lsn;

    if (lsnp != NULL)
        *lsnp = put_args.lsn;

    return (0);
}

/*
 * __disagg_put_crypt_key --
 *     Write encryption key data to disaggregated storage.
 */
static int
__disagg_put_crypt_key(
  WT_SESSION_IMPL *session, uint64_t page_id, const WT_ITEM *item, uint64_t *lsnp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGGREGATED_STORAGE *disagg;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    WT_ASSERT_ALWAYS(session, page_id <= WT_DISAGG_KEY_PROVIDER_MAX_PAGE_ID,
      "Multiple key provider pages is not currently supported");

    WT_RET(__disagg_put_page(session, disagg->page_log_key_provider, page_id, item,
      disagg->last_key_provider_page_lsn, lsnp));

    return (0);
}

/*
 * __disagg_put_meta --
 *     Write metadata to disaggregated storage.
 */
static int
__disagg_put_meta(WT_SESSION_IMPL *session, uint64_t page_id, const WT_ITEM *item, uint64_t *lsnp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGGREGATED_STORAGE *disagg;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    WT_ASSERT_ALWAYS(session, page_id <= WT_DISAGG_METADATA_MAX_PAGE_ID,
      "Metadata page ID %" PRIu64 " out of range", page_id);

    WT_RET(__disagg_put_page(
      session, disagg->page_log_meta, page_id, item, disagg->last_metadata_page_lsn, lsnp));
    ++disagg->num_meta_put;

    return (0);
}

/*
 * __disagg_set_crypt_header --
 *     Pack and byte-swap the crypt header information into the struct. Note: This function is not
 *     idempotent.
 */
static void
__disagg_set_crypt_header(WT_SESSION_IMPL *session, WT_CRYPT_KEYS *crypt)
{
    WT_CRYPT_HEADER *crypt_header = (WT_CRYPT_HEADER *)crypt->keys.mem;

    WT_ASSERT(session, crypt->keys.data != NULL);
    /* Prepare the crypt header. */
    crypt_header->signature = WT_CRYPT_HEADER_SIGNATURE;
    crypt_header->version = WT_CRYPT_HEADER_VERSION;
    crypt_header->compatible_version = WT_CRYPT_HEADER_COMPATIBLE_VERSION;
    crypt_header->header_size = sizeof(WT_CRYPT_HEADER);
    crypt_header->crypt_size = (uint32_t)crypt->keys.size;
    crypt_header->checksum = 0;

    __wt_crypt_header_byteswap(crypt_header);
    crypt->keys.data = crypt->keys.mem;
    crypt->keys.size += sizeof(WT_CRYPT_HEADER);

    /* Calculate checksum on both data and header. */
    crypt_header->checksum = __wt_checksum(crypt->keys.data, crypt->keys.size);
#ifdef WORDS_BIGENDIAN
    crypt_header->checksum = __wt_bswap32(crypt_header->checksum);
#endif
}

/*
 * __wt_disagg_set_database_size --
 *     Set the database size in disaggregated storage.
 */
void
__wt_disagg_set_database_size(WT_SESSION_IMPL *session, uint64_t database_size)
{
    S2C(session)->disaggregated_storage.database_size = database_size;
    WT_STAT_CONN_SET(session, disagg_database_size, database_size);
}

/*
 * __wt_disagg_put_crypt_helper --
 *     If new encryption key data information is detected, update the metadata page log and callback
 *     to the key provider upon completion.
 */
int
__wt_disagg_put_crypt_helper(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_CRYPT_KEYS crypt;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_KEY_PROVIDER *key_provider;
    uint64_t lsn;

    conn = S2C(session);
    key_provider = conn->key_provider;
    WT_CLEAR(crypt.keys);
    lsn = 0;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    if (session->ckpt.crash_trigger_point == KEY_PROVIDER_CRASH_BEFORE_KEY_ROTATION)
        __wt_debug_crash(session);

    /* Check for a new encryption key data. If the size is 0, there is none so we can skip. */
    WT_ERR(key_provider->get_key(key_provider, (WT_SESSION *)session, &crypt));
    if (crypt.keys.size == 0)
        goto done;

    /* WiredTiger has the memory ownership of the encryption key buffer. */
    WT_ERR(__wt_scr_alloc(session, crypt.keys.size + sizeof(WT_CRYPT_HEADER), &buf));
    crypt.keys.mem = buf->mem;
    crypt.keys.memsize = buf->memsize;
    crypt.keys.data = (uint8_t *)crypt.keys.mem + sizeof(WT_CRYPT_HEADER);

    /* Call the function again to fetch the new encryption key data. */
    WT_ERR(key_provider->get_key(key_provider, (WT_SESSION *)session, &crypt));
    WT_ASSERT(session, crypt.keys.size != 0 && crypt.keys.data != NULL);

    /* Pack the crypt header information into the struct. */
    __disagg_set_crypt_header(session, &crypt);

    /* Write the encryption key data to disaggregated storage. */
    ret = __disagg_put_crypt_key(session, WT_DISAGG_KEY_PROVIDER_MAIN_PAGE_ID, &crypt.keys, &lsn);

    if (session->ckpt.crash_trigger_point == KEY_PROVIDER_CRASH_DURING_KEY_ROTATION)
        __wt_debug_crash(session);

    /* Callback to update key provider on the result of new encryption key data . */
    if (ret == 0) {
        /* Point to the same encryption data on callback. */
        crypt.keys.data = (uint8_t *)crypt.keys.mem + sizeof(WT_CRYPT_HEADER);
        crypt.keys.size -= sizeof(WT_CRYPT_HEADER);
        crypt.r.lsn = lsn;
    } else {
        crypt.r.error = ret;
        /* On error, remove references of crypt key before calling back. */
        crypt.keys.data = NULL;
        crypt.keys.size = 0;
    }
    WT_IGNORE_RET(key_provider->on_key_update(key_provider, (WT_SESSION *)session, &crypt));

    if (session->ckpt.crash_trigger_point == KEY_PROVIDER_CRASH_AFTER_KEY_ROTATION)
        __wt_debug_crash(session);
done:
err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_disagg_put_checkpoint_meta --
 *     Write checkpoint information to the metadata page log and do the relevant bookkeeping.
 */
int
__wt_disagg_put_checkpoint_meta(WT_SESSION_IMPL *session, const char *checkpoint_root,
  size_t checkpoint_root_size, uint64_t checkpoint_timestamp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(metadata_buf);
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *disagg;
    wt_timestamp_t oldest_timestamp;
    uint64_t lsn;
    uint32_t checksum;
    char *checkpoint_root_copy, ts_string[2][WT_TS_INT_STRING_SIZE];

    checkpoint_root_copy = NULL;
    conn = S2C(session);
    disagg = &conn->disaggregated_storage;
    lsn = 0;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    if (checkpoint_root == NULL) {
        WT_ASSERT(session, checkpoint_root_size == 0);
        checkpoint_root = "";
    }
    if (checkpoint_root_size == 0)
        checkpoint_root_size = strlen(checkpoint_root);

    WT_ERR(__wt_strndup(session, checkpoint_root, checkpoint_root_size, &checkpoint_root_copy));
    WT_ERR(__wt_scr_alloc(session, 0, &metadata_buf));

    /*
     * Get the oldest timestamp from the metadata, don't use the one from the global transaction
     * structure as we need the timestamp associated with the checkpoint.
     */
    WT_ERR(__wt_meta_read_checkpoint_oldest(session, NULL, &oldest_timestamp, NULL));

    /* Format metadata settings. */
    WT_ERR(
      __wt_buf_fmt(session, metadata_buf,
        "checkpoint=%s,\n"
        "timestamp=%" PRIx64 ",\n"
        "oldest_timestamp=%" PRIx64,
        checkpoint_root_copy, checkpoint_timestamp, oldest_timestamp));

    /* Append key provider metadata, if available. */
    if (conn->key_provider != NULL) {
        /*
         * The key provider LSN field should always be initialized. The LSN is provided either
         * during startup, or when we detect a new encryption key.
         */
        WT_ASSERT(session,
          conn->disaggregated_storage
              .last_key_provider_page_lsn[WT_DISAGG_KEY_PROVIDER_MAIN_PAGE_ID] != 0);

        WT_ERR(__wt_buf_catfmt(session, metadata_buf,
          ",\n"
          "key_provider=(page.1=(page_id=%d,lsn=%" PRIu64 "),version=1)",
          WT_DISAGG_KEY_PROVIDER_MAIN_PAGE_ID,
          conn->disaggregated_storage
            .last_key_provider_page_lsn[WT_DISAGG_KEY_PROVIDER_MAIN_PAGE_ID]));
    }

    /* Compute the checksum for the metadata page. */
    checksum = __wt_checksum(metadata_buf->data, metadata_buf->size);

    /*
     * Write the metadata to disaggregated storage. This should be the last statement in this
     * function that is allowed to fail.
     */
    WT_ERR(__disagg_put_meta(session, WT_DISAGG_METADATA_MAIN_PAGE_ID, metadata_buf, &lsn));

    /*
     * Do the bookkeeping. We cannot fail this function past this point, so that our bookkeeping is
     * correct and self-consistent.
     */
    __wt_atomic_store_uint64_release(&disagg->last_checkpoint_meta_lsn, lsn);
    __wt_atomic_store_uint64_release(&disagg->last_checkpoint_timestamp, checkpoint_timestamp);
    __wt_atomic_store_uint64_release(&disagg->last_checkpoint_oldest_timestamp, oldest_timestamp);
    disagg->last_checkpoint_meta_checksum = checksum; /* Protected by the checkpoint lock. */

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Wrote disaggregated checkpoint metadata: lsn=%" PRIu64 ", timestamp=%" PRIu64
      " %s, oldest_timestamp=%" PRIu64 " %s, checksum=%" PRIx32 ", root=\"%s\"",
      lsn, checkpoint_timestamp, __wt_timestamp_to_string(checkpoint_timestamp, ts_string[0]),
      oldest_timestamp, __wt_timestamp_to_string(oldest_timestamp, ts_string[1]), checksum,
      checkpoint_root_copy);

    __wt_free(session, disagg->last_checkpoint_root);
    disagg->last_checkpoint_root = checkpoint_root_copy;
    checkpoint_root_copy = NULL;

err:
    __wt_free(session, checkpoint_root_copy);
    __wt_scr_free(session, &metadata_buf);

    return (ret);
}

/*
 * __wti_disagg_parse_crypt_meta --
 *     Parse key provider metadata to extract page ID and LSN.
 */
int
__wti_disagg_parse_crypt_meta(
  WT_SESSION_IMPL *session, const WT_DISAGG_METADATA *metadata, uint64_t *page_idp, uint64_t *lsnp)
{
    WT_CONFIG meta_cfg, page_cfg;
    WT_CONFIG_ITEM cfg_key, cfg_value;
    WT_DECL_RET;
    unsigned int version;

    WT_CLEAR(meta_cfg);
    WT_CLEAR(page_cfg);
    version = 0u;

    *page_idp = 0;
    *lsnp = 0;

    __wt_config_initn(session, &meta_cfg, metadata->key_provider, metadata->key_provider_len);
    while ((ret = __wt_config_next(&meta_cfg, &cfg_key, &cfg_value)) == 0) {
        if (WT_CONFIG_LIT_MATCH("page.1", cfg_key)) {
            __wt_config_subinit(session, &page_cfg, &cfg_value);
            while ((ret = __wt_config_next(&page_cfg, &cfg_key, &cfg_value)) == 0) {
                if (WT_CONFIG_LIT_MATCH("page_id", cfg_key) &&
                  cfg_value.type == WT_CONFIG_ITEM_NUM) {
                    WT_ASSERT_ALWAYS(
                      session, *page_idp == 0, "Duplicate page_id entry in key_provider metadata");
                    *page_idp = (uint64_t)cfg_value.val;
                } else if (WT_CONFIG_LIT_MATCH("lsn", cfg_key) &&
                  cfg_value.type == WT_CONFIG_ITEM_NUM) {
                    WT_ASSERT_ALWAYS(
                      session, *lsnp == 0, "Duplicate lsn entry in key_provider metadata");
                    *lsnp = (uint64_t)cfg_value.val;
                } else {
                    WT_ERR_MSG(session, EINVAL,
                      "Unknown or invalid entry \"%.*s\"=\"%.*s\" in key_provider page metadata",
                      (int)cfg_key.len, cfg_key.str, (int)cfg_value.len, cfg_value.str);
                }
            }
            WT_ERR_NOTFOUND_OK(ret, false);
        } else if (WT_CONFIG_LIT_MATCH("version", cfg_key) &&
          cfg_value.type == WT_CONFIG_ITEM_NUM) {
            version = (unsigned int)cfg_value.val;
        } else {
            WT_ERR_MSG(session, EINVAL,
              "Unknown or invalid entry \"%.*s\"=\"%.*s\" in key_provider metadata",
              (int)cfg_key.len, cfg_key.str, (int)cfg_value.len, cfg_value.str);
        }
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    if (version != 1) {
        WT_ERR_MSG(session, EINVAL, "Unsupported key_provider metadata version: %u", version);
    }

    if (*page_idp == 0 || *lsnp == 0) {
        WT_ERR_MSG(session, EINVAL,
          "Incomplete key_provider metadata: page_id=%" PRIu64 ", lsn=%" PRIu64, *page_idp, *lsnp);
    }

    if (*page_idp > WT_DISAGG_KEY_PROVIDER_MAX_PAGE_ID) {
        WT_ERR_MSG(session, EINVAL, "Key provider page ID %" PRIu64 " out of range", *page_idp);
    }

err:
    return (ret);
}

/*
 * __disagg_load_crypt_key --
 *     Load encryption key data from disaggregated storage into the key provider.
 */
static int
__disagg_load_crypt_key(WT_SESSION_IMPL *session, WT_DISAGG_METADATA *metadata)
{
    WT_CONNECTION_IMPL *conn;
    WT_CRYPT_HEADER *crypt_header;
    WT_CRYPT_KEYS crypt;
    WT_DECL_RET;
    WT_ITEM key_item;
    WT_KEY_PROVIDER *key_provider;

    conn = S2C(session);
    key_provider = conn->key_provider;

    WT_CLEAR(crypt);
    WT_CLEAR(key_item);

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    /* No key provider configured. */
    if (key_provider == NULL)
        return (0);

    /*
     * No key provider information stored in disaggregated storage. Use empty crypt keys to let the
     * key provider decide about the appropriate key.
     */
    if (metadata->key_provider == NULL) {
        WT_ERR(key_provider->load_key(key_provider, (WT_SESSION *)session, &crypt));
        return (0);
    }

    /* Parse crypt key metadata to get page ID and LSN. */
    uint64_t page_id, lsn;
    WT_ERR(__wti_disagg_parse_crypt_meta(session, metadata, &page_id, &lsn));

    /* Read the encryption key data from disaggregated storage. */
    WT_ERR(__disagg_get_crypt_key(session, page_id, lsn, &key_item));

    /* Validate the crypt data. */
    WT_ERR(__disagg_validate_crypt(session, &key_item, &crypt_header));

    /* Prepare the crypt keys for loading. */
    crypt.keys.data = (uint8_t *)key_item.data + crypt_header->header_size;
    crypt.keys.size = crypt_header->crypt_size;
    crypt.r.lsn = lsn;

    /* Callback to load the encryption key data into the key provider. */
    WT_ERR(key_provider->load_key(key_provider, (WT_SESSION *)session, &crypt));

err:
    __wt_buf_free(session, &key_item);
    return (ret);
}

/*
 * __disagg_fetch_shared_meta --
 *     Fetch the checkpoint metadata page, validate it, and return a zero-terminated buffer copy.
 */
static int
__disagg_fetch_shared_meta(
  WT_SESSION_IMPL *session, const WT_DISAGG_CHECKPOINT_META *ckpt_meta, WT_ITEM *item)
{
    WT_DECL_RET;

    /* Read the checkpoint metadata of the shared metadata table from the special metadata page. */
    WT_ERR_MSG_CHK(session,
      __disagg_get_meta(session, WT_DISAGG_METADATA_MAIN_PAGE_ID, ckpt_meta->metadata_lsn, item),
      "Disagg metadata fetching failed, with lsn: %" PRIu64, ckpt_meta->metadata_lsn);

    /* Validate the checksum. */
    if (ckpt_meta->has_metadata_checksum) {
        const uint32_t checksum = __wt_checksum(item->data, item->size);
        if (checksum != ckpt_meta->metadata_checksum) {
            WT_ERR_MSG(session, EIO,
              "Checkpoint metadata checksum mismatch: expected %" PRIx32 ", got %" PRIx32,
              ckpt_meta->metadata_checksum, checksum);
        }
    }

err:
    return (ret);
}

/* !!!
 * __disagg_parse_legacy_meta --
 *     Parse legacy metadata pulled from the shared metadata buffer. Note: No allocations performed
 *     during the parsing. Resulting WT_DISAGG_METADATA fields will point into meta_buf.
 *
 *     The legacy format is a new-line-separated pair of records:
 *
 *     (WiredTigerCheckpoint.1=(...))\n
 *     timestamp=hhhh
 */
static int
__disagg_parse_legacy_meta(
  WT_SESSION_IMPL *session, const WT_ITEM *meta_buf, WT_DISAGG_METADATA *metadata)
{
    WT_CONFIG_ITEM timestamp;
    WT_DECL_RET;
    const char *s = (const char *)meta_buf->data;
    const char *meta_end = NULL;

    WT_CLEAR(timestamp);
    WT_CLEAR(*metadata);
    metadata->checkpoint_timestamp = WT_TS_MAX; /* Invalid timestamp by default. */

    /* Find the end of the first line. */
    meta_end = strchr(s, '\n');
    if (meta_end == NULL) {
        WT_ERR_MSG(session, EINVAL,
          "Disaggregated checkpoint legacy metadata missing timestamp entry: \"%.*s\"",
          (int)meta_buf->size, (const char *)meta_buf->data);
    }
    metadata->checkpoint = s;
    metadata->checkpoint_len = (size_t)(meta_end - s);

    s = meta_end + 1; /* Move past the newline */

    /* Parse the timestamp line. */
    if (!WT_PREFIX_MATCH(s, "timestamp=")) {
        WT_ERR_MSG(session, EINVAL,
          "Disaggregated checkpoint legacy metadata invalid timestamp entry: \"%.*s\"",
          (int)meta_buf->size, (const char *)meta_buf->data);
    }

    WT_PREFIX_SKIP_REQUIRED(session, s, "timestamp=");
    timestamp.str = s;
    timestamp.len = meta_buf->size - (size_t)(s - (const char *)meta_buf->data);

    if (timestamp.len == 0)
        WT_ERR_MSG(session, EINVAL,
          "Disaggregated checkpoint legacy metadata missing timestamp value: \"%.*s\"",
          (int)meta_buf->size, (const char *)meta_buf->data);

    WT_ERR(__wt_conf_parse_hex(
      session, "checkpoint timestamp", &metadata->checkpoint_timestamp, &timestamp));

err:
    return (ret);
}

/* !!!
 * __disagg_parse_meta --
 *     Parse metadata pulled from the shared metadata buffer. Note: No allocations performed during
 *     the parsing. Resulting WT_DISAGG_METADATA fields will point into meta_buf.
 *
 *     Metadata format follows the regular config format. Example:
 *
 *     checkpoint=(WiredTigerCheckpoint.1=(addr="00c025808282bd21596019", order=1, ...)),
 *     timestamp=0,
 *     key_provider=(page.1=(page_id=1,lsn=123),version=1)
 */
static int
__disagg_parse_meta(WT_SESSION_IMPL *session, const WT_ITEM *meta_buf, WT_DISAGG_METADATA *metadata)
{
    WT_CONFIG meta_cfg;
    WT_CONFIG_ITEM cfg_key, cfg_value;
    WT_DECL_RET;

    WT_CLEAR(meta_cfg);
    WT_CLEAR(*metadata);
    metadata->checkpoint_timestamp = WT_TS_MAX; /* Invalid timestamp by default. */

    __wt_config_initn(session, &meta_cfg, meta_buf->data, meta_buf->size);
    while ((ret = __wt_config_next(&meta_cfg, &cfg_key, &cfg_value)) == 0) {
        __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Disaggregated checkpoint metadata item \"%.*s\"=\"%.*s\"", (int)cfg_key.len, cfg_key.str,
          (int)cfg_value.len, cfg_value.str);

        if (WT_CONFIG_LIT_MATCH("checkpoint", cfg_key)) {
            WT_ASSERT_ALWAYS(session, metadata->checkpoint == NULL,
              "Duplicate checkpoint entry in disaggregated storage metadata");

            metadata->checkpoint = cfg_value.str;
            metadata->checkpoint_len = cfg_value.len;
        } else if (WT_CONFIG_LIT_MATCH("timestamp", cfg_key)) {
            WT_ASSERT_ALWAYS(session, metadata->checkpoint_timestamp == WT_TS_MAX,
              "Duplicate timestamp entry in disaggregated storage metadata");

            if (cfg_value.len > 0 && cfg_value.val == 0)
                metadata->checkpoint_timestamp = WT_TS_NONE;
            else
                WT_ERR(__wt_txn_parse_timestamp(
                  session, "checkpoint timestamp", &metadata->checkpoint_timestamp, &cfg_value));
        } else if (WT_CONFIG_LIT_MATCH("oldest_timestamp", cfg_key)) {
            WT_ASSERT_ALWAYS(session, metadata->oldest_timestamp == WT_TS_NONE,
              "Duplicate timestamp entry in disaggregated storage metadata: "
              "metadata->oldest_timestamp=%" PRIu64,
              metadata->oldest_timestamp);

            if (cfg_value.len > 0 && cfg_value.val == 0)
                metadata->oldest_timestamp = WT_TS_NONE;
            else
                WT_ERR(__wt_txn_parse_timestamp(
                  session, "oldest timestamp", &metadata->oldest_timestamp, &cfg_value));
        } else if (WT_CONFIG_LIT_MATCH("key_provider", cfg_key)) {
            WT_ASSERT_ALWAYS(session, metadata->key_provider == NULL,
              "Duplicate key_provider entry in disaggregated storage metadata");

            metadata->key_provider = cfg_value.str;
            metadata->key_provider_len = cfg_value.len;
        } else {
            WT_ERR_MSG(session, EINVAL, "Unknown entry \"%.*s\" in disaggregated storage metadata",
              (int)cfg_key.len, cfg_key.str);
        }
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    return (ret);
}

/*
 * __wt_disagg_parse_meta --
 *     Parse metadata pulled from the shared metadata buffer. Note: No allocations performed during
 *     the parsing. Resulting WT_DISAGG_METADATA fields will point into meta_buf.
 */
int
__wt_disagg_parse_meta(
  WT_SESSION_IMPL *session, const WT_ITEM *meta_buf, WT_DISAGG_METADATA *metadata)
{
    WT_DECL_RET;

    if (meta_buf->size == 0)
        WT_ERR_MSG(session, EINVAL, "Disaggregated checkpoint metadata is empty");

    WT_CLEAR(*metadata);
    metadata->checkpoint_timestamp = WT_TS_MAX; /* Invalid timestamp by default. */

    if (WT_PREFIX_MATCH((const char *)meta_buf->data, "checkpoint=")) {
        __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Disaggregated checkpoint metadata starts with \"checkpoint=\";"
          "Parsing regular format. Found \"%.*s\"",
          (int)meta_buf->size, (const char *)meta_buf->data);
        WT_ERR(__disagg_parse_meta(session, meta_buf, metadata));

    } else {
        __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Disaggregated checkpoint metadata does not start with \"checkpoint=\";"
          "Parsing legacy format. Found \"%.*s\"",
          (int)meta_buf->size, (const char *)meta_buf->data);
        WT_ERR(__disagg_parse_legacy_meta(session, meta_buf, metadata));
    }

    if (metadata->checkpoint == NULL)
        WT_ERR_MSG(session, EINVAL, "Missing checkpoint entry in disaggregated storage metadata");
    if (metadata->checkpoint_timestamp == WT_TS_MAX)
        WT_ERR_MSG(session, EINVAL, "Missing timestamp entry in disaggregated storage metadata");
    /* Key provider entry is optional. */

err:
    return (ret);
}

/*
 * __disagg_save_checkpoint_meta --
 *     Update the local metadata entry with the supplied checkpoint configuration.
 */
static int
__disagg_save_checkpoint_meta(WT_SESSION_IMPL *session, WT_SESSION_IMPL *internal_session,
  WT_CURSOR *md_cursor, const WT_DISAGG_METADATA *metadata)
{
    WT_DECL_ITEM(metadata_cfg);
    WT_DECL_RET;
    char *cfg_ret;
    const char *cfg[3], *current_value, *metadata_key;

    cfg_ret = NULL;
    metadata_key = WT_DISAGG_METADATA_URI;

    /* Pull the value out. */
    md_cursor->set_key(md_cursor, metadata_key);
    WT_ERR(md_cursor->search(md_cursor));
    WT_ERR(md_cursor->get_value(md_cursor, &current_value));

    /* Create the new checkpoint config string. */
    WT_ERR(__wt_scr_alloc(session, 0, &metadata_cfg));
    WT_ERR(__wt_buf_fmt(session, metadata_cfg, "checkpoint=%.*s", (int)metadata->checkpoint_len,
      metadata->checkpoint));

    cfg[0] = current_value;
    cfg[1] = metadata_cfg->data;
    cfg[2] = NULL;
    WT_ERR(__wt_config_collapse(session, cfg, &cfg_ret));

    /* Put our new config in */
    WT_ERR(__wt_metadata_insert(internal_session, metadata_key, cfg_ret));

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Updated the local metadata for key \"%s\" to include the new checkpoint: \"%.*s\"",
      metadata_key, (int)metadata->checkpoint_len, metadata->checkpoint);

err:
    __wt_free(session, cfg_ret);
    __wt_scr_free(session, &metadata_cfg);
    return (ret);
}

/*
 * __disagg_apply_checkpoint_meta --
 *     Process the metadata entries stored in the shared metadata table for a new checkpoint.
 */
static int
__disagg_apply_checkpoint_meta(
  WT_SESSION_IMPL *session, WT_CURSOR *md_cursor, const WT_DISAGG_CHECKPOINT_META *ckpt_meta)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR *cursor;
    WT_DECL_ITEM(metadata_cfg);
    WT_DECL_ITEM(old_uri_buf);
    WT_DECL_RET;
    uint32_t existing_tables, new_tables, new_ingest;
    char *layered_ingest_uri, *cfg_ret;
    const char *cfg[3], *checkpoint_name, *checkpoint_name_new, *current_value, *metadata_key,
      *metadata_value;

    cursor = NULL;
    checkpoint_name = NULL;
    checkpoint_name_new = NULL;
    layered_ingest_uri = cfg_ret = NULL;
    existing_tables = new_tables = new_ingest = 0;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->schema_lock);

    /*
     * Throw away any references to the old disaggregated metadata table. This ensures that we are
     * on the most recent checkpoint from now on.
     */
    WT_WITHOUT_DHANDLE(session, ret = __wti_conn_dhandle_outdated(session, WT_DISAGG_METADATA_URI));
    WT_ERR_MSG_CHK(session, ret, "Removing old references to disagg tables failed: \"%s\"",
      WT_DISAGG_METADATA_URI);

    __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Processing new disaggregated storage checkpoint: metadata_lsn=%" PRIu64,
      ckpt_meta->metadata_lsn);

    cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_open_cursor);
    cfg[1] = NULL;
    WT_ERR(__wt_open_cursor(session, WT_DISAGG_METADATA_URI, NULL, cfg, &cursor));

    WT_ERR(__wt_scr_alloc(session, 0, &metadata_cfg));
    WT_ERR(__wt_scr_alloc(session, 0, &old_uri_buf));

    while ((ret = cursor->next(cursor)) == 0) {
        WT_ERR(cursor->get_key(cursor, &metadata_key));
        WT_ERR(cursor->get_value(cursor, &metadata_value));

        md_cursor->set_key(md_cursor, metadata_key);
        WT_ERR_NOTFOUND_OK(md_cursor->search(md_cursor), true);

        if (ret == 0 && WT_PREFIX_MATCH(metadata_key, "file:")) {
            /* Existing table: Just apply the new metadata. */
            WT_ERR(__wt_config_getones(session, metadata_value, "checkpoint", &cval));
            WT_ERR(__wt_buf_fmt(session, metadata_cfg, "checkpoint=%.*s", (int)cval.len, cval.str));

            /* Merge the new checkpoint metadata into the current table metadata. */
            WT_ERR(md_cursor->get_value(md_cursor, &current_value));
            cfg[0] = current_value;
            cfg[1] = metadata_cfg->data;
            cfg[2] = NULL;
            WT_ERR(__wt_config_collapse(session, cfg, &cfg_ret));

            int64_t order, order_new;
            uint64_t time, time_new;
            /*
             * Before inserting the new value, get the checkpoint name of the file at the previous
             * checkpoint.
             */
            WT_ERR_NOTFOUND_OK(__wt_meta_checkpoint_last_name(
                                 session, metadata_key, &checkpoint_name, &order, &time),
              false);

            /* Retrieve the name of the current unnamed checkpoint. */
            checkpoint_name_new = NULL;
            order_new = 0;
            time_new = 0;
            WT_ERR_NOTFOUND_OK(__wt_ckpt_last_name(session, metadata_value, &checkpoint_name_new,
                                 &order_new, &time_new),
              false);
            WT_ERR_MSG_CHK(session, ret,
              "Retrieving the last checkpoint name failed for key \"%s\"", metadata_key);

            /* FIXME-WT-14730: check that the other parts of the metadata are identical. */
            /*
             * FIXME-WT-16494: how to decide two checkpoints are different if they are written by
             * different nodes.
             */
            bool same_checkpoint = checkpoint_name != NULL && checkpoint_name_new != NULL &&
              strcmp(checkpoint_name, checkpoint_name_new) == 0 && order == order_new &&
              time == time_new;

            /* Put our new config in */
            md_cursor->set_value(md_cursor, cfg_ret);
            WT_ERR_MSG_CHK(session, md_cursor->insert(md_cursor),
              "Failed to insert metadata for key \"%s\"", metadata_key);

            ++existing_tables;
            __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Updated the local metadata for key \"%s\" to include new checkpoint: \"%.*s\"",
              metadata_key, (int)cval.len, cval.str);

            /*
             * Mark any matching data handles associated with the previous checkpoint to be out of
             * date. Any new opens will get the new metadata.
             */
            if (!same_checkpoint) {
                WT_ERR(__wt_buf_fmt(session, old_uri_buf, "%s/%s", metadata_key, checkpoint_name));
                WT_WITHOUT_DHANDLE(
                  session, ret = __wti_conn_dhandle_outdated(session, old_uri_buf->data));
                WT_ERR_MSG_CHK(session, ret, "Marking data handles outdated failed: \"%s\"",
                  (const char *)old_uri_buf->data);
            }
            /*
             * Mark all live btrees as outdated. Otherwise, we will not open a new dhandle for live
             * btrees after step-up.
             *
             * TODO: This is better done at step-up or step-down to force close all live btrees.
             */
            WT_WITHOUT_DHANDLE(session, ret = __wti_conn_dhandle_outdated(session, metadata_key));
            WT_ERR_MSG_CHK(session, ret, "Marking data handles outdated failed: \"%s\"",
              (const char *)metadata_key);
            __wt_free(session, cfg_ret);
            __wt_free(session, checkpoint_name);
            __wt_free(session, checkpoint_name_new);
        } else if (ret == WT_NOTFOUND) {
            /* New table: Insert new metadata. */
            /* FIXME-WT-14730: verify that there is no btree ID conflict. */

            /* Create the corresponding ingest table, if it does not exist. */
            if (WT_PREFIX_MATCH(metadata_key, "layered:")) {
                WT_ERR(__wt_config_getones(session, metadata_value, "ingest", &cval));
                if (cval.len > 0) {
                    WT_ERR(__wt_calloc_def(session, cval.len + 1, &layered_ingest_uri));
                    memcpy(layered_ingest_uri, cval.str, cval.len);
                    layered_ingest_uri[cval.len] = '\0';
                    md_cursor->set_key(md_cursor, layered_ingest_uri);
                    WT_ERR_NOTFOUND_OK(md_cursor->search(md_cursor), true);
                    if (ret == WT_NOTFOUND) {
                        WT_ERR_MSG_CHK(session,
                          __layered_create_missing_ingest_table(
                            session, layered_ingest_uri, metadata_value),
                          "Failed to create missing ingest table \"%s\" from \"%s\"",
                          layered_ingest_uri, metadata_value);
                        new_ingest++;
                    }
                    __wt_free(session, layered_ingest_uri);
                    layered_ingest_uri = NULL;
                }
                new_tables++;
            }

            /* Insert the actual metadata. */
            md_cursor->set_key(md_cursor, metadata_key);
            md_cursor->set_value(md_cursor, metadata_value);
            WT_ERR_MSG_CHK(session, md_cursor->insert(md_cursor),
              "Failed to insert metadata for key \"%s\"", metadata_key);

            __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Inserted new key to the local metadata \"%s\": \"%s\"", metadata_key,
              metadata_value);
        }
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Checkpoint pickup processed %" PRIu32 " existing tables, %" PRIu32 " new tables, %" PRIu32
      " new ingest tables",
      existing_tables, new_tables, new_ingest);

err:
    __wt_free(session, cfg_ret);
    __wt_free(session, checkpoint_name);
    __wt_free(session, checkpoint_name_new);
    __wt_free(session, layered_ingest_uri);
    __wt_scr_free(session, &metadata_cfg);
    __wt_scr_free(session, &old_uri_buf);
    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));
    return (ret);
}

/*
 * __disagg_update_checkpoint_meta --
 *     Finalize checkpoint bookkeeping after processing shared metadata entries.
 */
static int
__disagg_update_checkpoint_meta(WT_SESSION_IMPL *session, WT_SESSION_IMPL *internal_session,
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
      &conn->disaggregated_storage.last_checkpoint_timestamp, metadata->checkpoint_timestamp);
    __wt_atomic_store_uint64_release(
      &conn->disaggregated_storage.last_checkpoint_oldest_timestamp, metadata->oldest_timestamp);

    /* Set the database size. */
    if (ckpt_meta->has_database_size)
        __wt_disagg_set_database_size(session, ckpt_meta->database_size);

    /* Remember the root config of the last checkpoint. */
    __wt_free(session, conn->disaggregated_storage.last_checkpoint_root);
    WT_ERR(__wt_strndup(session, metadata->checkpoint, metadata->checkpoint_len,
      &conn->disaggregated_storage.last_checkpoint_root));

    /* Update ingest tables' prune timestamps. */
    WT_ERR_MSG_CHK(session,
      __wti_layered_iterate_ingest_tables_for_gc_pruning(
        internal_session, metadata->checkpoint_timestamp),
      "Updating prune timestamp failed");

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
    WT_CURSOR *md_cursor;
    WT_DECL_RET;
    WT_DISAGG_METADATA metadata;
    WT_ITEM metadata_buf;
    WT_SESSION_IMPL *internal_session;
    uint64_t current_meta_lsn;
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    conn = S2C(session);

    WT_CLEAR(ts_string);
    WT_CLEAR(metadata_buf);
    WT_CLEAR(metadata);
    internal_session = NULL;
    md_cursor = NULL;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

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

    __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Picking up disaggregated storage checkpoint: metadata_lsn=%" PRIu64,
      ckpt_meta->metadata_lsn);

    /*
     * Part 1: Get the metadata of the shared metadata table and insert it into our metadata table.
     */

    WT_ERR(__disagg_fetch_shared_meta(session, ckpt_meta, &metadata_buf));
    WT_ERR(__wt_disagg_parse_meta(session, &metadata_buf, &metadata));

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Picking up disaggregated storage checkpoint: metadata_lsn=%" PRIu64 ", timestamp=%" PRIu64
      " %s"
      ", oldest_timestamp=%" PRIu64 " %s, root=\"%.*s\"",
      ckpt_meta->metadata_lsn, metadata.checkpoint_timestamp,
      __wt_timestamp_to_string(metadata.checkpoint_timestamp, ts_string[0]),
      metadata.oldest_timestamp, __wt_timestamp_to_string(metadata.oldest_timestamp, ts_string[1]),
      (int)metadata.checkpoint_len, metadata.checkpoint);

    /* Load crypt key data with the key provider extension, if any. */
    WT_ERR(__disagg_load_crypt_key(session, &metadata));

    /* We need an internal session when modifying metadata. */
    WT_ERR(__wt_open_internal_session(conn, "checkpoint-pick-up", false, 0, 0, &internal_session));

    /* Open up a metadata cursor pointing at our table */
    WT_ERR(__wt_metadata_cursor(internal_session, &md_cursor));

    /* Update our local metadata with the new checkpoint entry. */
    WT_ERR(__disagg_save_checkpoint_meta(session, internal_session, md_cursor, &metadata));

    /*
     * Part 2: Apply the metadata for other tables from the shared metadata table. FIXME-WT-16528
     * Investigate whether we need a separate internal session to pick up the new checkpoint.
     */
    WT_WITH_SCHEMA_LOCK(internal_session,
      ret = __disagg_apply_checkpoint_meta(internal_session, md_cursor, ckpt_meta));
    WT_ERR(ret);

    /*
     * Part 3: Do the bookkeeping.
     */

    WT_ERR(__disagg_update_checkpoint_meta(session, internal_session, ckpt_meta, &metadata));

    /* Log the completion of the checkpoint pick-up. */
    __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Finished picking up disaggregated storage checkpoint: metadata_lsn=%" PRIu64,
      ckpt_meta->metadata_lsn);

err:
    if (ret == 0)
        WT_STAT_CONN_INCR(session, layered_table_manager_checkpoints_disagg_pick_up_succeed);
    else {
        WT_STAT_CONN_INCR(session, layered_table_manager_checkpoints_disagg_pick_up_failed);
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_ERROR,
          "Failed to pick up disaggregated storage checkpoint for metadata_lsn=%" PRIu64 ": ret=%d",
          ckpt_meta->metadata_lsn, ret);
    }

    if (md_cursor != NULL)
        WT_TRET(__wt_metadata_cursor_release(internal_session, &md_cursor));

    if (internal_session != NULL)
        WT_TRET(__wt_session_close_internal(internal_session));

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
 *     Pick up a new checkpoint from metadata config.
 */
static int
__disagg_pick_up_checkpoint_meta(
  WT_SESSION_IMPL *session, const char *meta_data, size_t meta_data_size)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_DISAGG_CHECKPOINT_META ckpt_meta;
    uint64_t metadata_checksum;
    char *meta_str;

    WT_CLEAR(ckpt_meta);
    meta_str = NULL;

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

    /* Extract the database size, if it exists. */
    WT_ERR_NOTFOUND_OK(__wt_config_getones(session, meta_str, "database_size", &cval), true);
    if (WT_CHECK_AND_RESET(ret, 0) && cval.len != 0) {
        /*
         * FIXME-WT-16562 Checkpoint size tech debt cleanup. Disagg checkpoint metadata may be
         * received without database_size. For now we treat this field as optional to avoid crashing
         * when size information is missing. Once checkpoint size support is fully established, this
         * fallback path should be removed and database_size made mandatory.
         */
        ckpt_meta.has_database_size = true;
        ckpt_meta.database_size = (uint64_t)cval.val;
    }
    /* Parse and validate version and compatible_version fields. */
    WT_ERR(__disagg_check_meta_version(session, meta_str, &ckpt_meta));

    /* Now actually pick up the checkpoint. */
    WT_ERR(__disagg_pick_up_checkpoint(session, &ckpt_meta));

err:
    __wt_free(session, meta_str);
    return (ret);
}

/*
 * __disagg_shared_metadata_queue_free --
 *     Free an entry in the update metadata queue.
 */
static void
__disagg_shared_metadata_queue_free(WT_SESSION_IMPL *session, WT_DISAGG_METADATA_OP **entry)
{
    if (*entry == NULL)
        return;
    __wt_free(session, (*entry)->stable_uri);
    __wt_free(session, (*entry)->table_name);
    __wt_free(session, (*entry)->colgroup_value);
    __wt_free(session, (*entry)->layered_value);
    __wt_free(session, (*entry)->stable_value);
    __wt_free(session, (*entry)->table_value);
    __wt_free(session, *entry);
    *entry = NULL;
}

/*
 * __shared_metadata_op_to_string --
 *     Convert a metadata operation to string representation.
 */
static inline const char *
__shared_metadata_op_to_string(WT_SHARED_METADATA_OP op)
{
    switch (op) {
    case WT_SHARED_METADATA_UPDATE:
        return ("UPDATE");
    case WT_SHARED_METADATA_CREATE:
        return ("CREATE");
    case WT_SHARED_METADATA_REMOVE:
        return ("REMOVE");
    }
    return ("UNKNOWN");
}

/*
 * __disagg_save_metadata --
 *     Fetch a metadata key/value pair from the metadata table and save the value.
 */
static int
__disagg_save_metadata(WT_SESSION_IMPL *session, WT_CURSOR *md_cursor, const char *prefix,
  const char *key, char **valuep)
{
    WT_DECL_ITEM(md_key);
    WT_DECL_RET;
    const char *md_value;

    WT_ERR(__wt_scr_alloc(session, 0, &md_key));
    WT_ERR(__wt_buf_fmt(session, md_key, "%s%s", prefix, key));

    md_cursor->set_key(md_cursor, md_key->data);
    WT_ERR_NOTFOUND_OK(md_cursor->search(md_cursor), true);
    if (!WT_CHECK_AND_RESET(ret, WT_NOTFOUND)) {
        WT_ERR(md_cursor->get_value(md_cursor, &md_value));
        WT_ERR(__wt_strdup(session, md_value, valuep));
    }

err:
    __wt_scr_free(session, &md_key);
    return (ret);
}

/*
 * __wt_disagg_enqueue_metadata_operation --
 *     Enqueue a metadata operation for a given URI into the shared metadata table to be done at the
 *     next checkpoint.
 */
int
__wt_disagg_enqueue_metadata_operation(WT_SESSION_IMPL *session, const char *stable_uri,
  const char *table_name, WT_SHARED_METADATA_OP metadata_op)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_DISAGG_METADATA_OP *entry;
    bool ckpt_running;

    conn = S2C(session);
    cursor = NULL;
    entry = NULL;

    /*
     * Ensure that the schema lock is held. We cannot check this via spinlock ownership, because
     * this function might be called from an internal session, while the lock was acquired by its
     * parent session.
     */
    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA));

    /* Allocate the entry structure. */
    WT_ERR(__wt_calloc_one(session, &entry));
    entry->metadata_op = metadata_op;
    WT_ERR(__wt_strdup(session, stable_uri, &entry->stable_uri));
    WT_ERR(__wt_strdup(session, table_name, &entry->table_name));

    /* Get the table metadata. */
    WT_ERR(__wt_metadata_cursor(session, &cursor));

    /* Fetch the relevant data from the metadata table and save it in the entry. */
    WT_ERR(
      __disagg_save_metadata(session, cursor, "colgroup:", table_name, &entry->colgroup_value));
    WT_ERR(__disagg_save_metadata(session, cursor, "layered:", table_name, &entry->layered_value));
    WT_ERR(__disagg_save_metadata(session, cursor, "table:", table_name, &entry->table_value));
    WT_ERR(__disagg_save_metadata(session, cursor, "", stable_uri, &entry->stable_value));

    /*
     * When WiredTiger is running a checkpoint, prevent drop updates from entering the shared
     * metadata table for that checkpoint. We defer these metadata operations to the next checkpoint
     * to keep the checkpoints metadata and table state consistent.
     */
    WT_ACQUIRE_READ_WITH_BARRIER(ckpt_running, conn->txn_global.checkpoint_running);
    if (ckpt_running && (metadata_op == WT_SHARED_METADATA_REMOVE))
        entry->deferred = true;

    /* Cannot fail past this point. */
    __wt_spin_lock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);
    TAILQ_INSERT_TAIL(&conn->disaggregated_storage.shared_metadata_qh, entry, q);
    __wt_spin_unlock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Scheduled copying disaggregated metadata for table \"%s\" (stable URI \"%s\") with %s "
      "operation to shared "
      "metadata table at next checkpoint:",
      table_name, stable_uri, __shared_metadata_op_to_string(metadata_op));
    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "  colgroup: %s",
      entry->colgroup_value == NULL ? "<none>" : entry->colgroup_value);
    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "  layered: %s",
      entry->layered_value == NULL ? "<none>" : entry->layered_value);
    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "  table: %s",
      entry->table_value == NULL ? "<none>" : entry->table_value);
    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "  stable: %s",
      entry->stable_value == NULL ? "<none>" : entry->stable_value);

    /* No need to free the entry structure here as it has been added to the queue. */
    entry = NULL;

err:
    __disagg_shared_metadata_queue_free(session, &entry);

    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __disagg_shared_metadata_queue_clear --
 *     Clear the update metadata list.
 */
static void
__disagg_shared_metadata_queue_clear(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGG_METADATA_OP *entry, *tmp;

    conn = S2C(session);

    __wt_spin_lock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    WT_TAILQ_SAFE_REMOVE_BEGIN(entry, &conn->disaggregated_storage.shared_metadata_qh, q, tmp)
    {
        TAILQ_REMOVE(&conn->disaggregated_storage.shared_metadata_qh, entry, q);
        __disagg_shared_metadata_queue_free(session, &entry);
    }
    WT_TAILQ_SAFE_REMOVE_END

    __wt_spin_unlock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);
}

/*
 * __disagg_shared_metadata_op_helper --
 *     Perform the remove/update operation in the shared metadata table.
 */
static int
__disagg_shared_metadata_op_helper(
  WT_SESSION_IMPL *session, const char *key, const char *value, WT_SHARED_METADATA_OP metadata_op)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "overwrite", NULL};

    WT_ASSERT(session, S2C(session)->layered_table_manager.leader);

    cursor = NULL;

    WT_ERR(__wt_open_cursor(session, WT_DISAGG_METADATA_URI, NULL, cfg, &cursor));
    cursor->set_key(cursor, key);

    switch (metadata_op) {
    case WT_SHARED_METADATA_REMOVE:
        /*
         * Layered tables can be created via two methods. When created with the "table:" prefix, we
         * expect metadata entries for layered, colgroup, table, and file. When created with the
         * "layered:" prefix, we expect only layered and file metadata entries.
         *
         * Since either form may appear depending on how the table was created, it is acceptable for
         * some lookups to return WT_NOTFOUND.
         */
        WT_ERR_NOTFOUND_OK(cursor->remove(cursor), false);
        break;
    case WT_SHARED_METADATA_CREATE:
    case WT_SHARED_METADATA_UPDATE:
        if (value == NULL) {
            ret = 0;
            goto err;
        }

        cursor->set_value(cursor, value);
        WT_ERR(cursor->insert(cursor));
        break;
    }

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "%s disaggregated shared metadata: key=\"%s\" value=\"%s\"",
      __shared_metadata_op_to_string(metadata_op), key, value);

err:
    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));
    return (ret);
}

/*
 * __disagg_shared_metadata_op --
 *     Remove/update all relevant metadata entries of a table in the shared metadata table.
 */
static int
__disagg_shared_metadata_op(WT_SESSION_IMPL *session, WT_DISAGG_METADATA_OP *entry)
{
    WT_DECL_ITEM(md_key);
    WT_DECL_RET;

    WT_ERR(__wt_scr_alloc(session, 0, &md_key));
    WT_ERR(__wt_buf_fmt(session, md_key, "colgroup:%s", entry->table_name));
    WT_ERR(__disagg_shared_metadata_op_helper(
      session, md_key->data, entry->colgroup_value, entry->metadata_op));

    WT_ERR(__wt_buf_fmt(session, md_key, "layered:%s", entry->table_name));
    WT_ERR(__disagg_shared_metadata_op_helper(
      session, md_key->data, entry->layered_value, entry->metadata_op));

    WT_ERR(__wt_buf_fmt(session, md_key, "table:%s", entry->table_name));
    WT_ERR(__disagg_shared_metadata_op_helper(
      session, md_key->data, entry->table_value, entry->metadata_op));

    WT_ERR(__disagg_shared_metadata_op_helper(
      session, entry->stable_uri, entry->stable_value, entry->metadata_op));
err:
    __wt_scr_free(session, &md_key);
    return (ret);
}

/*
 * __wt_disagg_shared_metadata_queue_process --
 *     Process the update metadata list.
 */
int
__wt_disagg_shared_metadata_queue_process(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGG_METADATA_OP *entry, *tmp;

    conn = S2C(session);

    /*
     * This requires schema lock to ensure that we capture a consistent snapshot of metadata entries
     * related to the given shared table, e.g., the various file, colgroup, table, and layered
     * entries.
     */
    WT_ASSERT_SPINLOCK_OWNED(session, &conn->schema_lock);

    __wt_spin_lock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    TAILQ_FOREACH_SAFE(entry, &conn->disaggregated_storage.shared_metadata_qh, q, tmp)
    {
        if (entry->deferred) {
            __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Defer metadata %s operation for table \"%s\"", entry->table_name,
              __shared_metadata_op_to_string(entry->metadata_op));
            entry->deferred = false;
            continue;
        }

        WT_ERR(__disagg_shared_metadata_op(session, entry));

        TAILQ_REMOVE(&conn->disaggregated_storage.shared_metadata_qh, entry, q);
        __disagg_shared_metadata_queue_free(session, &entry);
    }

err:
    __wt_spin_unlock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    return (ret);
}

/*
 * __disagg_metadata_table_init --
 *     Initialize the shared metadata table.
 */
static int
__disagg_metadata_table_init(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *internal_session;

    conn = S2C(session);

    WT_ERR(__wt_open_internal_session(conn, "disagg-init", false, 0, 0, &internal_session));
    WT_ERR(__wt_session_create(
      internal_session, WT_DISAGG_METADATA_URI, "key_format=S,value_format=S,log=(enabled=false)"));

err:
    if (internal_session != NULL)
        WT_TRET(__wt_session_close_internal(internal_session));
    return (ret);
}

/*
 * __wti_disagg_set_last_materialized_lsn --
 *     Set the latest materialized LSN.
 */
int
__wti_disagg_set_last_materialized_lsn(WT_SESSION_IMPL *session, uint64_t lsn)
{
    WT_DISAGGREGATED_STORAGE *disagg;
    uint64_t cur_lsn;

    disagg = &S2C(session)->disaggregated_storage;
    cur_lsn = __wt_atomic_load_uint64_acquire(&disagg->last_materialized_lsn);

    if (cur_lsn > lsn)
        return (EINVAL); /* Can't go backwards. */

    __wt_atomic_store_uint64_release(&disagg->last_materialized_lsn, lsn);
    return (0);
}

/*
 * __disagg_abandon_checkpoint --
 *     Abandon the current incomplete checkpoint, if the operation is supported by the provided PALI
 *     implementation. It is a no-op if the operation is not supported, in which case the
 *     application must either perform the equivalent operation before changing roles, or otherwise
 *     guarantee that no updates have been made since the last completed checkpoint.
 *
 * If there are any updates after the last completed checkpoint beyond this point, performing any
 *     writes to the disaggregated tables may lead to undefined behavior, such as illegal delta
 *     chains with wrong backlink LSNs, committing updates from incomplete checkpoints, or even data
 *     loss in the case of not cleaning up abandoned page discards.
 */
static int
__disagg_abandon_checkpoint(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGGREGATED_STORAGE *disagg;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    /* Only the leader can abandon a checkpoint. */
    if (disagg->npage_log == NULL || !conn->layered_table_manager.leader)
        WT_RET(EINVAL);

    /*
     * FIXME-WT-16524: This function is no longer an optional operation for testing, remove this
     * check.
     */
    if (disagg->npage_log->page_log->pl_abandon_checkpoint == NULL) {
        __wt_verbose_warning(session, WT_VERB_DISAGGREGATED_STORAGE, "%s",
          "Abandon checkpoint operation is not supported by the current PALI implementation");
        return (0);
    }

    /*
     * Call the PALI function to abandon the checkpoint. Since we are not specifying the latest
     * complete checkpoint, the implementation of this function would identify the LSN of the last
     * checkpoint completion record and drop all later records. If there are no more updates after
     * the last complete checkpoint, the function would have no effect.
     */
    WT_RET(disagg->npage_log->page_log->pl_abandon_checkpoint(
      disagg->npage_log->page_log, &session->iface));

    return (0);
}

/*
 * __disagg_begin_checkpoint --
 *     Begin the next checkpoint.
 */
static int
__disagg_begin_checkpoint(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGGREGATED_STORAGE *disagg;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    /* Only the leader can begin a global checkpoint. */
    if (disagg->npage_log == NULL || !conn->layered_table_manager.leader)
        return (0);

    /* On fresh startup, load an empty key to key provider. */
    if (conn->key_provider != NULL) {
        WT_DISAGG_METADATA metadata = {0};
        WT_RET(__disagg_load_crypt_key(session, &metadata));
    }

    WT_RET(disagg->npage_log->page_log->pl_begin_checkpoint(
      disagg->npage_log->page_log, &session->iface, 0));

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Begin next disaggregated storage checkpoint: num_meta_put=%" PRIu64, disagg->num_meta_put);

    /* Store is sufficient because updates are protected by the checkpoint lock. */
    disagg->num_meta_put_at_ckpt_begin = disagg->num_meta_put;
    return (0);
}

/*
 * __disagg_restart_checkpoint --
 *     Restart the current checkpoint: Abandon the current checkpoint if it is incomplete (and the
 *     operation to abandon a checkpoint is supported), and begin a new checkpoint.
 */
static int
__disagg_restart_checkpoint(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->checkpoint_lock);

    WT_ERR_MSG_CHK(
      session, __disagg_abandon_checkpoint(session), "Failed to abandon the incomplete checkpoint");
    WT_ERR_MSG_CHK(session, __disagg_begin_checkpoint(session), "Failed to begin a new checkpoint");

err:
    return (ret);
}

/*
 * __disagg_step_up --
 *     Step up to the node to the leader mode.
 */
static int
__disagg_step_up(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *internal_session;

    conn = S2C(session);

    /*
     * Some functionality in stepping up needs a session that can open data handles. The default
     * session used to call this function cannot do that.
     */
    WT_RET(__wt_open_internal_session(conn, "disagg-step-up", false, 0, 0, &internal_session));

    /*
     * We need to hold the checkpoint lock while stepping up, because if we change the role
     * concurrently with a checkpoint, it would do only a part of the work required for the new
     * role, leaving the database in an inconsistent state.
     */
    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    __wt_verbose_debug1(
      session, WT_VERB_DISAGGREGATED_STORAGE, "%s", "Stepping up to the leader mode");
    F_SET(conn, WT_CONN_RECONFIGURING_STEP_UP);

    /*
     * Step up to the leader mode. We need to do this first, because the rest of the operations
     * below depend on WiredTiger already being in the leader mode.
     */
    conn->layered_table_manager.leader = true;
    WT_STAT_CONN_SET(session, disagg_role_leader, 1);

    /*
     * Abandon the current checkpoint if it is incomplete, and begin a new one. We need to do this
     * before draining the ingest tables, so that the updates to the stable tables will be correctly
     * included in the new checkpoint.
     */
    WT_ERR(__disagg_restart_checkpoint(session));

    /*
     * We might not need to hold a checkpoint lock below this point, but we will keep it just to be
     * safe. If this becomes a problem, we can revisit whether we really need to hold the lock for
     * the remaining operations.
     */

    /* Create any missing stable tables. */
    WT_ERR_MSG_CHK(session, __layered_create_missing_stable_tables(internal_session),
      "Failed to create missing stable tables");

    /* Drain the ingest tables before switching to leader. */
    WT_ERR_MSG_CHK(session, __wti_layered_drain_ingest_tables(internal_session),
      "Failed to drain ingest tables");

err:
    WT_TRET(__wt_session_close_internal(internal_session));
    F_CLR(conn, WT_CONN_RECONFIGURING_STEP_UP);
    return (ret);
}

/*
 * __disagg_step_down --
 *     Step down to the follower mode.
 */
static void
__disagg_step_down(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    __wt_verbose_debug1(
      session, WT_VERB_DISAGGREGATED_STORAGE, "%s", "Stepping down to the follower mode");

    conn->layered_table_manager.leader = false;
    WT_STAT_CONN_SET(session, disagg_role_leader, 0);

    /* Do some cleanup as we are abandoning the current checkpoint. */
    __disagg_shared_metadata_queue_clear(session);
}

/*
 * __wti_disagg_conn_config --
 *     Parse and setup the disaggregated server options for the connection.
 */
int
__wti_disagg_conn_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ITEM complete_checkpoint_meta;
    WT_NAMED_PAGE_LOG *npage_log;
    uint64_t time_start, time_stop;
    bool leader, picked_up, was_leader;

    conn = S2C(session);
    leader = was_leader = conn->layered_table_manager.leader;
    npage_log = NULL;
    picked_up = false;

    WT_CLEAR(complete_checkpoint_meta);

    /* Reconfigure-only settings. */
    if (reconfig) {

        /* Pick up a new checkpoint (followers only). */
        WT_ERR_NOTFOUND_OK(
          __wt_config_gets(session, cfg, "disaggregated.checkpoint_meta", &cval), true);
        if (ret == 0 && cval.len > 0) {
            /*
             * FIXME-WT-14733: currently the leader silently ignores the checkpoint_meta
             * configuration as it may have an obsolete configuration in its base config when it is
             * still a follower.
             */
            if (!leader) {
                WT_WITH_CHECKPOINT_LOCK(
                  session, ret = __disagg_pick_up_checkpoint_meta(session, cval.str, cval.len));
                WT_ERR_MSG_CHK(session, ret, "Failed to pick up a new checkpoint with config: %.*s",
                  (int)cval.len, cval.str);
            }
        }
    }

    /* Common settings between initial connection config and reconfig. */

    /* Get the last materialized LSN. */
    /* FIXME-WT-15447 Consider deprecating this. */
    WT_ERR_NOTFOUND_OK(
      __wt_config_gets(session, cfg, "disaggregated.last_materialized_lsn", &cval), true);
    if (ret == 0 && cval.len > 0 && cval.val >= 0)
        WT_ERR_MSG_CHK(session, __wti_disagg_set_last_materialized_lsn(session, (uint64_t)cval.val),
          "Failed to set the last materialized LSN to %" PRIu64, (uint64_t)cval.val);

    /* Set the role. */
    WT_ERR(__wt_config_gets(session, cfg, "disaggregated.role", &cval));
    if (cval.len == 0 || WT_CONFIG_LIT_MATCH("follower", cval))
        leader = false;
    else if (WT_CONFIG_LIT_MATCH("leader", cval))
        leader = true;
    else
        WT_ERR_MSG(session, EINVAL, "Invalid node role");

    if (!reconfig) {
        /* Set the initial role. */
        conn->layered_table_manager.leader = leader;
        WT_STAT_CONN_SET(session, disagg_role_leader, leader ? 1 : 0);
    } else if (!was_leader && leader) {
        /* Follower step-up. */
        time_start = __wt_clock(session);
        WT_WITH_CHECKPOINT_LOCK(session, ret = __disagg_step_up(session));
        time_stop = __wt_clock(session);
        WT_ERR_MSG_CHK(session, ret, "Failed to step up to the leader role");
        WT_STAT_CONN_SET(session, disagg_step_up_time, WT_CLOCKDIFF_MS(time_stop, time_start));
        __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Step up completed in %" PRIu64 " milliseconds", WT_CLOCKDIFF_MS(time_stop, time_start));
    } else if (was_leader && !leader) {
        /* Leader step-down. */
        time_start = __wt_clock(session);
        WT_WITH_CHECKPOINT_LOCK(session, __disagg_step_down(session));
        time_stop = __wt_clock(session);
        WT_STAT_CONN_SET(session, disagg_step_down_time, WT_CLOCKDIFF_MS(time_stop, time_start));
        __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Step down completed in %" PRIu64 " milliseconds",
          WT_CLOCKDIFF_MS(time_stop, time_start));
    }
    /* Connection init settings only. */

    if (reconfig)
        goto err;

    /* Remember the configuration. */
    WT_ERR(__wt_config_gets(session, cfg, "disaggregated.page_log", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &conn->disaggregated_storage.page_log));

    /* Setup any configured page log. */
    WT_ERR(__wt_config_gets(session, cfg, "disaggregated.page_log", &cval));
    WT_ERR(__wt_schema_open_page_log(session, &cval, &npage_log));
    conn->disaggregated_storage.npage_log = npage_log;

    if (npage_log != NULL) {
        /* Set up a handle for accessing shared metadata. */
        WT_ERR(npage_log->page_log->pl_open_handle(npage_log->page_log, &session->iface,
          WT_SPECIAL_PALI_TURTLE_FILE_ID, &conn->disaggregated_storage.page_log_meta));

        /* Set up a handle for accessing the key provider table if configured. */
        if (conn->key_provider != NULL)
            WT_ERR(npage_log->page_log->pl_open_handle(npage_log->page_log, &session->iface,
              WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID,
              &conn->disaggregated_storage.page_log_key_provider));
    }

    /* FIXME-WT-14965: Exit the function immediately if this check returns false. */
    if (__wt_conn_is_disagg(session)) {
        WT_ERR(__wti_layered_table_manager_init(session));

        /* If we are starting as a primary, abandon a previous incomplete checkpoint. */
        if (leader) {
            WT_WITH_CHECKPOINT_LOCK(session, ret = __disagg_abandon_checkpoint(session));
            WT_ERR_MSG_CHK(session, ret, "Failed to abandon the incomplete checkpoint");
        }

        /* Initialize the shared metadata table. */
        WT_ERR(__disagg_metadata_table_init(session));

        /* Pick up the selected checkpoint. */
        WT_ERR_NOTFOUND_OK(
          __wt_config_gets(session, cfg, "disaggregated.checkpoint_meta", &cval), true);
        if (ret == 0 && cval.len > 0) {
            WT_WITH_CHECKPOINT_LOCK(
              session, ret = __disagg_pick_up_checkpoint_meta(session, cval.str, cval.len));
            WT_ERR_MSG_CHK(session, ret, "Failed to pick up a new checkpoint with config: %.*s",
              (int)cval.len, cval.str);
            picked_up = true;
        }

        /* If we are starting as primary (e.g., for internal testing), begin the checkpoint. */
        if (leader && !picked_up) {
            ret =
              __layered_get_disagg_checkpoint(session, cfg, NULL, NULL, &complete_checkpoint_meta);
            WT_ERR_NOTFOUND_OK(ret, true);
            if (ret == 0) {
                /* Pick up the checkpoint we just found. */
                WT_WITH_CHECKPOINT_LOCK(session,
                  ret = __disagg_pick_up_checkpoint_meta(
                    session, complete_checkpoint_meta.data, complete_checkpoint_meta.size));

                __wt_buf_free(session, &complete_checkpoint_meta);
                WT_ERR_MSG_CHK(session, ret, "Failed to pick up checkpoint metadata");
            } else if (WT_CHECK_AND_RESET(ret, WT_NOTFOUND))
                __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "%s",
                  "Did not find any complete checkpoint to pick up at startup");
            WT_WITH_CHECKPOINT_LOCK(session, ret = __disagg_begin_checkpoint(session));
            WT_ERR_MSG_CHK(session, ret, "Failed to begin a new checkpoint");
        }

        WT_ERR(__wt_config_gets(session, cfg, "page_delta.internal_page_delta", &cval));
        if (cval.val != 0)
            F_SET(&conn->page_delta, WT_INTERNAL_PAGE_DELTA);

        WT_ERR(__wt_config_gets(session, cfg, "page_delta.leaf_page_delta", &cval));
        if (cval.val != 0)
            F_SET(&conn->page_delta, WT_LEAF_PAGE_DELTA);

        WT_ERR(__wt_config_gets(session, cfg, "disaggregated.lose_all_my_data", &cval));
        if (cval.val != 0)
            F_SET(&conn->disaggregated_storage, WT_DISAGG_NO_SYNC);

        /*
         * Get the percentage of a page size that a delta must be less than in order to write that
         * delta (instead of just giving up and writing the full page).
         */
        WT_ERR(__wt_config_gets(session, cfg, "page_delta.delta_pct", &cval));
        if (cval.len > 0 && cval.val >= 0)
            conn->page_delta.delta_pct = (uint32_t)cval.val;

        /* Get the maximum number of consecutive deltas allowed for a single page. */
        WT_ERR(__wt_config_gets(session, cfg, "page_delta.max_consecutive_delta", &cval));
        if (cval.len > 0 && cval.val >= 0)
            conn->page_delta.max_consecutive_delta = (uint32_t)cval.val;

        /* Get the number of threads used to drain the ingest tables. */
        WT_ERR(__wt_config_gets(session, cfg, "disaggregated.drain_threads", &cval));
        if (cval.len > 0 && cval.val >= 0)
            conn->layered_drain_data.thread_count = (uint32_t)cval.val;
    }

err:
    /* Dump available logged errors into the event handler to ease debugging. */
    if (ret != 0)
        __wt_error_log_to_handler(session);

    if (ret != 0 && reconfig && !was_leader && leader)
        return (__wt_panic(session, ret, "failed to step-up as primary"));
    return (ret);
}

/*
 * __wt_conn_is_disagg --
 *     Check whether the connection uses disaggregated storage.
 */
bool
__wt_conn_is_disagg(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGGREGATED_STORAGE *disagg;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    return (disagg->page_log_meta != NULL);
}

/*
 * __on_file_in_wt_dir --
 *     Act on a file in WT directory: delete or fail depending on the flag.
 */
static int
__on_file_in_wt_dir(WT_SESSION_IMPL *session, const char *fname, bool fail)
{
    if (fail)
        WT_RET_MSG(session, EEXIST,
          "Disaggregated storage requires a clean directory, but found WiredTiger file %s: "
          "use 'disaggregated.local_files_action=delete' to remove it.",
          fname);

    __wt_verbose_warning(
      session, WT_VERB_METADATA, "Removing local file due to disagg mode: %s", fname);
    WT_RET(__wt_fs_remove(session, fname, false, false));

    return (0);
}

/*
 * __ensure_clean_startup_dir --
 *     Check for local files in a directory that need to be removed before starting in disaggregated
 *     mode.
 */
static int
__ensure_clean_startup_dir(WT_SESSION_IMPL *session, const char *dir, bool fail)
{
    WT_DECL_RET;

    if (*dir != '\0') {
        bool exists;
        WT_RET(__wt_fs_exist(session, dir, &exists));
        if (!exists)
            return (0); /* Nothing to do, directory does not exist. */
    }

    u_int file_count = 0;
    char **files = NULL;
    WT_ERR(__wt_fs_directory_list(session, dir, "", &files, &file_count));

    if (file_count <= 0)
        goto err;

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

#ifndef _WIN32
    { /* Limit the scope of big local stack variables. */
        char cwd[MAXPATHLEN];
        if (getcwd(cwd, MAXPATHLEN) == NULL) {
            cwd[0] = '?';
            cwd[1] = '\0';
        }
        __wt_verbose_debug1(session, WT_VERB_METADATA,
          "Found %u local files in directory <%s> -> <%s>:", file_count, cwd, dir);
    }
#endif

    for (u_int i = 0; i < file_count; i++) {
        /* Build full file name */
        char full_path_buf[MAXPATHLEN];
        char *full_path;
        if (dir[0] != '\0') {
            WT_ERR(__wt_snprintf(full_path_buf, sizeof(full_path_buf), "%s%s%s", dir,
              __wt_path_separator(), files[i]));
            full_path = full_path_buf;
        } else
            full_path = files[i];

        struct stat sb;
        if (stat(full_path, &sb) == 0) {
            __wt_verbose_debug1(session, WT_VERB_METADATA,
              "File:  %s: size=%" WT_SIZET_FMT " mode=%03o uid=%u gid=%u mtime=%s", full_path,
              (size_t)sb.st_size, (u_int)sb.st_mode & 0777, (u_int)sb.st_uid, (u_int)sb.st_gid,
              ctime(&sb.st_mtime));
        } else
            __wt_verbose_debug1(
              session, WT_VERB_METADATA, "  %s: stat failed: %s", full_path, strerror(errno));

        /*
         * Delete any WiredTiger files to prevent reading them during startup. But keep
         * WiredTiger.lock as a safety mechanism.
         */
        if (WT_PREFIX_MATCH(files[i], "WiredTiger") && !WT_STREQ(files[i], WT_SINGLETHREAD))
            WT_ERR(__on_file_in_wt_dir(session, full_path, fail));
        else if (WT_SUFFIX_MATCH(files[i], ".wt") || WT_SUFFIX_MATCH(files[i], ".wt_ingest") ||
          WT_SUFFIX_MATCH(files[i], ".wt_stable"))
            /*
             * Delete all normal tables since they are not usable without metadata anyway.
             *
             * Delete ingest and stable tables as they are not guaranteed to be consistent. If they
             * are not deleted now, the files will be renamed and kept around - someone will have to
             * clean them up later.
             */
            WT_ERR(__on_file_in_wt_dir(session, full_path, fail));
        else
            __wt_verbose_debug1(session, WT_VERB_METADATA, "Keeping local file: %s", full_path);
    }

err:
    WT_TRET(__wt_fs_directory_list_free(session, &files, file_count));
    return (ret);
}

/*
 * __wti_ensure_clean_startup_dir --
 *     Check for local files that need to be removed before starting in disaggregated mode.
 *
 * Disaggregated storage needs to start with a clean directory, for now wipe out the directory if
 *     starting in disaggregated storage mode. Eventually this should not be necessary but at the
 *     moment WiredTiger will generate local files in disaggregated storage mode, and MongoDB
 *     expects to be able to restart without files being present.
 *
 * FIXME-WT-15163: Revisit what files get written and what needs to be deleted.
 */
int
__wti_ensure_clean_startup_dir(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;

    /*
     * FIXME-WT-14721: As it stands, __wt_conn_is_disagg only works after we have metadata access,
     * which depends on having run recovery, so the config hack is the simplest way to break that
     * dependency.
     */
    WT_RET(__wt_config_gets(session, cfg, "disaggregated.page_log", &cval));
    if (cval.len == 0)
        return (0); /* Not in disaggregated mode, nothing to do. */
    WT_RET(__wt_config_gets(session, cfg, "disaggregated.lose_all_my_data", &cval));
    if (cval.val == 0)
        return (0);

    /*
     * Possible actions for local files are: fail, delete, ignore.
     *
     * A reasonable default for Disagg would be to delete all local WT-related files, since they can
     * be in an inconsistent state anyway. Since this only works together with the
     * "lose_all_my_data" option, it's considered to be safe enough to be triggered by accident.
     */
    bool fail;
    WT_RET(__wt_config_gets(session, cfg, "disaggregated.local_files_action", &cval));
    if (WT_CONFIG_LIT_MATCH("fail", cval))
        fail = true;
    else if (WT_CONFIG_LIT_MATCH("ignore", cval))
        return (0);
    else
        fail = false; /* Default: delete */

    /* Delete from home directory. */
    WT_RET(__ensure_clean_startup_dir(session, "", fail));

    /*
     * Delete from log directory.
     *
     * Since log manager is not initialized yet, read directly from config.
     */
    const char *log_path;
    WT_RET(__wt_config_gets(session, cfg, "log.path", &cval));
    if (cval.len > 0 && !(cval.len == 1 && cval.str[0] == '.')) {
        WT_RET(__wt_strndup(session, cval.str, cval.len, &log_path));
        ret = __ensure_clean_startup_dir(session, log_path, fail);
        __wt_free(session, log_path);
    }

    return (ret);
}

/*
 * __wti_disagg_destroy --
 *     Shut down disaggregated storage.
 */
int
__wti_disagg_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *disagg;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    /* Remove the list of URIs for which we still need to update metadata entries. */
    __disagg_shared_metadata_queue_clear(session);

    /* Close the metadata handles. */
    if (disagg->page_log_meta != NULL) {
        WT_TRET(disagg->page_log_meta->plh_close(disagg->page_log_meta, &session->iface));
        disagg->page_log_meta = NULL;
    }

    /* Close the key provider handle. */
    if (disagg->page_log_key_provider != NULL) {
        WT_TRET(
          disagg->page_log_key_provider->plh_close(disagg->page_log_key_provider, &session->iface));
        disagg->page_log_key_provider = NULL;
    }

    __wt_free(session, disagg->last_checkpoint_root);
    __wt_free(session, disagg->page_log);
    return (ret);
}

/*
 * __wt_disagg_advance_checkpoint --
 *     Advance to the next checkpoint. If the current checkpoint is 0, just start the next one.
 */
int
__wt_disagg_advance_checkpoint(WT_SESSION_IMPL *session, bool ckpt_success)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(meta);
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *disagg;
    wt_timestamp_t checkpoint_timestamp;
    uint64_t meta_lsn;
    uint32_t meta_checksum;
    char ts_string[WT_TS_INT_STRING_SIZE];

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    /* Only the leader can advance the global checkpoint. */
    if (disagg->npage_log == NULL || !conn->layered_table_manager.leader)
        return (0);

    WT_RET(__wt_scr_alloc(session, 0, &meta));

    /* Get the checksum of the metadata page. This access is protected by the checkpoint lock. */
    meta_checksum = conn->disaggregated_storage.last_checkpoint_meta_checksum;

    /* The following accesses are read from atomic variables. */
    meta_lsn =
      __wt_atomic_load_uint64_acquire(&conn->disaggregated_storage.last_checkpoint_meta_lsn);
    checkpoint_timestamp =
      __wt_atomic_load_uint64_acquire(&conn->disaggregated_storage.cur_checkpoint_timestamp);
    WT_ASSERT(session, meta_lsn > 0); /* The metadata page should be written by now. */

    if (ckpt_success) {
        /*
         * Important: To keep testing simple, keep the metadata to be a valid configuration string
         * without quotation marks or escape characters.
         */
        WT_ERR(__wt_buf_fmt(session, meta,
          "metadata_lsn=%" PRIu64 ",metadata_checksum=%" PRIx32 ",database_size=%" PRIu64
          ",version=%d,compatible_version=%d",
          meta_lsn, meta_checksum, conn->disaggregated_storage.database_size,
          WT_DISAGG_CHECKPOINT_META_VERSION, WT_DISAGG_CHECKPOINT_META_COMPATIBLE_VERSION));
        WT_ERR(disagg->npage_log->page_log->pl_complete_checkpoint_ext(disagg->npage_log->page_log,
          &session->iface, 0, (uint64_t)checkpoint_timestamp, meta, NULL));
        __wt_atomic_store_uint64_release(
          &conn->disaggregated_storage.last_checkpoint_timestamp, checkpoint_timestamp);

        __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Completed disaggregated storage checkpoint: lsn=%" PRIu64 ", timestamp=%" PRIu64 " %s",
          meta_lsn, checkpoint_timestamp,
          __wt_timestamp_to_string(checkpoint_timestamp, ts_string));
    }

    WT_ERR(__disagg_begin_checkpoint(session));

err:
    __wt_scr_free(session, &meta);
    return (ret);
}

#ifdef HAVE_UNITTEST
void
__ut_disagg_set_crypt_header(WT_SESSION_IMPL *session, WT_CRYPT_KEYS *crypt)
{
    __disagg_set_crypt_header(session, crypt);
}

int
__ut_disagg_validate_crypt(WT_SESSION_IMPL *session, WT_ITEM *key_item, WT_CRYPT_HEADER **header)
{
    return (__disagg_validate_crypt(session, key_item, header));
}

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

void
__ut_disagg_get_crypt_header(WT_ITEM *key_item, WT_CRYPT_HEADER **header)
{
    __disagg_get_crypt_header(key_item, header);
}
#endif
