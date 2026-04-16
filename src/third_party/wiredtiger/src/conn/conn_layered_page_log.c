/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/* Function prototypes for disaggregated storage and layered tables. */
static void __disagg_set_crypt_header(WT_SESSION_IMPL *session, WT_CRYPT_KEYS *crypt);
static void __disagg_get_crypt_header(WT_ITEM *key_item, WT_CRYPT_HEADER **header);

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
 * __wti_layered_get_disagg_checkpoint --
 *     Get existing checkpoint information from disaggregated storage.
 */
int
__wti_layered_get_disagg_checkpoint(WT_SESSION_IMPL *session, const char **cfg,
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
 * __wti_disagg_load_crypt_key --
 *     Load encryption key data from disaggregated storage into the key provider.
 */
int
__wti_disagg_load_crypt_key(WT_SESSION_IMPL *session, WT_DISAGG_METADATA *metadata)
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
 * __disagg_get_crypt_header --
 *     Copy and byte-swap the crypt header from the key item. Note: This function is not idempotent.
 */
static void
__disagg_get_crypt_header(WT_ITEM *key_item, WT_CRYPT_HEADER **header)
{
    *header = (WT_CRYPT_HEADER *)key_item->data;
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
    if (ret != 0)
        __wt_verbose_error(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Failed to put new encryption key data to disaggregated storage: %d", ret);
    __wt_scr_free(session, &buf);
    return (ret);
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

/* Limit the amount of bytes we dump in the metadata page dump. */
#define WT_DISAGG_META_DUMP_MAX 1024

/*
 * __wti_disagg_fetch_shared_meta --
 *     Fetch the checkpoint metadata page, validate it, and return a zero-terminated buffer copy.
 */
int
__wti_disagg_fetch_shared_meta(
  WT_SESSION_IMPL *session, const WT_DISAGG_CHECKPOINT_META *ckpt_meta, WT_ITEM *item)
{
    WT_DECL_ITEM(hex_buf);
    WT_DECL_RET;

    /* Read the checkpoint metadata of the shared metadata table from the special metadata page. */
    WT_ERR_MSG_CHK(session,
      __disagg_get_meta(session, WT_DISAGG_METADATA_MAIN_PAGE_ID, ckpt_meta->metadata_lsn, item),
      "Disagg metadata fetching failed, with lsn: %" PRIu64, ckpt_meta->metadata_lsn);

    /* Validate the checksum. */
    if (ckpt_meta->has_metadata_checksum) {
        const uint32_t checksum = __wt_checksum(item->data, item->size);
        if (checksum != ckpt_meta->metadata_checksum) {
            const size_t dump_size =
              item->size > WT_DISAGG_META_DUMP_MAX ? WT_DISAGG_META_DUMP_MAX : item->size;
            WT_ERR(__wt_scr_alloc(session, 0, &hex_buf));
            WT_ERR_MSG(session, EIO,
              "Checkpoint metadata corruption detected: lsn=%" PRIu64 ", expected=0x%" PRIx32
              ", got=0x%" PRIx32 ", data=[%s]",
              ckpt_meta->metadata_lsn, ckpt_meta->metadata_checksum, checksum,
              __wt_buf_set_printable(session, item->data, dump_size, false, hex_buf));
        }
    }

err:
    __wt_scr_free(session, &hex_buf);
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
    uint32_t checksum, max_table_id;
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

    WT_WITH_SCHEMA_LOCK(session, max_table_id = conn->next_file_id);

    /* Format metadata settings. */
    WT_ERR(
      __wt_buf_fmt(session, metadata_buf,
        "version=%d,compatible_version=%d,\n"
        "checkpoint=%s,\n"
        "timestamp=%" PRIx64 ",\n"
        "oldest_timestamp=%" PRIx64 ",\n"
        "largest_file_id=%" PRIu32,
        WT_DISAGG_CHECKPOINT_TURTLE_VERSION, WT_DISAGG_CHECKPOINT_TURTLE_COMPATIBLE_VERSION,
        checkpoint_root_copy, checkpoint_timestamp, oldest_timestamp, max_table_id));

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
      " %s, oldest_timestamp=%" PRIu64 " %s, largest_file_id=%" PRIu32 ", checksum=%" PRIx32
      ", root=\"%s\"",
      lsn, checkpoint_timestamp, __wt_timestamp_to_string(checkpoint_timestamp, ts_string[0]),
      oldest_timestamp, __wt_timestamp_to_string(oldest_timestamp, ts_string[1]), max_table_id,
      checksum, checkpoint_root_copy);

    __wt_free(session, disagg->last_checkpoint_root);
    disagg->last_checkpoint_root = checkpoint_root_copy;
    checkpoint_root_copy = NULL;

err:
    if (ret == 0)
        __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "%s",
          "Updated disaggregated storage checkpoint metadata");
    else
        __wt_verbose_error(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Failed to write disaggregated checkpoint metadata: %d", ret);
    __wt_free(session, checkpoint_root_copy);
    __wt_scr_free(session, &metadata_buf);

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
    metadata->version = WT_DISAGG_CHECKPOINT_TURTLE_VERSION_DEFAULT;
    metadata->compatible_version = WT_DISAGG_CHECKPOINT_TURTLE_VERSION_DEFAULT;

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
 *     version=1,compatible_version=1,
 *     checkpoint=(WiredTigerCheckpoint.1=(addr="00c025808282bd21596019", order=1, ...)),
 *     timestamp=0,
 *     largest_file_id=0,
 *     key_provider=(page.1=(page_id=1,lsn=123),version=1)
 */
static int
__disagg_parse_meta(WT_SESSION_IMPL *session, const WT_ITEM *meta_buf, WT_DISAGG_METADATA *metadata)
{
    WT_CONFIG meta_cfg;
    WT_CONFIG_ITEM cfg_key, cfg_value;
    WT_DECL_RET;

    WT_CLEAR(meta_cfg);
    metadata->checkpoint_timestamp = WT_TS_MAX; /* Invalid timestamp by default. */

    __wt_config_initn(session, &meta_cfg, meta_buf->data, meta_buf->size);
    while ((ret = __wt_config_next(&meta_cfg, &cfg_key, &cfg_value)) == 0) {
        __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Disaggregated checkpoint metadata item \"%.*s\"=\"%.*s\"", (int)cfg_key.len, cfg_key.str,
          (int)cfg_value.len, cfg_value.str);

        if (WT_CONFIG_LIT_MATCH("version", cfg_key)) {
            /* Already parsed in version check pass, skip */
        } else if (WT_CONFIG_LIT_MATCH("compatible_version", cfg_key)) {
            /* Already parsed in version check pass, skip */
        } else if (WT_CONFIG_LIT_MATCH("checkpoint", cfg_key)) {
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
        } else if (WT_CONFIG_LIT_MATCH("largest_file_id", cfg_key)) {
            WT_ASSERT_ALWAYS(session, metadata->largest_file_id == 0,
              "Duplicate largest file entry in disaggregated storage metadata: "
              "metadata->largest_file_id=%" PRIu32,
              metadata->largest_file_id);

            if (cfg_value.len > 0)
                metadata->largest_file_id = (uint32_t)cfg_value.val;
        } else if (WT_CONFIG_LIT_MATCH("key_provider", cfg_key)) {
            WT_ASSERT_ALWAYS(session, metadata->key_provider == NULL,
              "Duplicate key_provider entry in disaggregated storage metadata");

            metadata->key_provider = cfg_value.str;
            metadata->key_provider_len = cfg_value.len;
        } else {
            /*
             * Unknown key is an error only for current metadata version. For non-current versions,
             * ignore for compatibility.
             */
            if (metadata->version == WT_DISAGG_CHECKPOINT_TURTLE_VERSION)
                WT_ERR_MSG(
                  session, EINVAL, "Unknown metadata entry: %.*s", (int)cfg_key.len, cfg_key.str);
        }
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    return (ret);
}

/*
 * __disagg_parse_version_and_check --
 *     Parse version and compatible_version fields from metadata and validate compatibility. Returns
 *     early with error if metadata requires a newer reader version.
 */
static int
__disagg_parse_version_and_check(
  WT_SESSION_IMPL *session, const WT_ITEM *meta_buf, WT_DISAGG_METADATA *metadata)
{
    WT_CONFIG_ITEM compat_val, version_val;
    WT_DECL_RET;
    bool has_compat, has_version;

    WT_CLEAR(version_val);
    WT_CLEAR(compat_val);

    metadata->version = WT_DISAGG_CHECKPOINT_TURTLE_VERSION_DEFAULT;
    metadata->compatible_version = WT_DISAGG_CHECKPOINT_TURTLE_VERSION_DEFAULT;

    WT_ERR_NOTFOUND_OK(
      __wt_config_getones_n(session, meta_buf->data, meta_buf->size, "version", &version_val),
      false);
    WT_ERR_NOTFOUND_OK(__wt_config_getones_n(session, meta_buf->data, meta_buf->size,
                         "compatible_version", &compat_val),
      false);

    has_version = version_val.len > 0;
    has_compat = compat_val.len > 0;

    if (has_version && !has_compat)
        WT_ERR_MSG(session, EINVAL,
          "Disaggregated checkpoint metadata with version %" PRId64 " missing compatible version",
          version_val.val);
    if (!has_version && has_compat)
        WT_ERR_MSG(session, EINVAL,
          "Disaggregated checkpoint metadata with compatible version %" PRId64 " missing version",
          compat_val.val);

    if (has_version && has_compat) {
        if (version_val.val < 0 || version_val.val > INT_MAX)
            WT_ERR_MSG(session, EINVAL, "Invalid version value: %" PRId64, version_val.val);
        if (compat_val.val < 0 || compat_val.val > INT_MAX)
            WT_ERR_MSG(
              session, EINVAL, "Invalid compatible_version value: %" PRId64, compat_val.val);

        metadata->version = (int)version_val.val;
        metadata->compatible_version = (int)compat_val.val;
    }

    if (metadata->compatible_version > WT_DISAGG_CHECKPOINT_TURTLE_VERSION)
        WT_ERR_MSG(session, ENOTSUP,
          "Disaggregated checkpoint metadata requires version greater or equal to %d, but current "
          "version is %d",
          metadata->compatible_version, WT_DISAGG_CHECKPOINT_TURTLE_VERSION);

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
    static const char *legacy_ckpt_prefix = "(WiredTigerCheckpoint.";

    if (meta_buf->size == 0)
        WT_ERR_MSG(session, EINVAL, "Disaggregated checkpoint metadata is empty");

    WT_CLEAR(*metadata);
    metadata->checkpoint_timestamp = WT_TS_MAX; /* Invalid timestamp by default. */

    /*
     * Detect format by checking for the legacy prefix. The legacy format always starts with
     * "(WiredTigerCheckpoint.", while the regular config format does not.
     */
    if (WT_PREFIX_MATCH((const char *)meta_buf->data, legacy_ckpt_prefix)) {
        __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Disaggregated checkpoint metadata starts with \"%s\";"
          "Parsing legacy format. Found \"%.*s\"",
          legacy_ckpt_prefix, (int)meta_buf->size, (const char *)meta_buf->data);
        WT_ERR(__disagg_parse_legacy_meta(session, meta_buf, metadata));

    } else {
        __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Performing version check for disaggregated checkpoint metadata. Found \"%.*s\"",
          (int)meta_buf->size, (const char *)meta_buf->data);
        WT_ERR(__disagg_parse_version_and_check(session, meta_buf, metadata));
        __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Disaggregated checkpoint metadata does not start with \"%s\";"
          "Parsing regular format. Found \"%.*s\"",
          legacy_ckpt_prefix, (int)meta_buf->size, (const char *)meta_buf->data);
        WT_ERR(__disagg_parse_meta(session, meta_buf, metadata));
    }

    if (metadata->checkpoint == NULL)
        WT_ERR_MSG(session, EINVAL, "Missing checkpoint entry in disaggregated storage metadata");
    if (metadata->checkpoint_timestamp == WT_TS_MAX)
        WT_ERR_MSG(session, EINVAL, "Missing timestamp entry in disaggregated storage metadata");
    /* Key provider entry is optional. */

err:
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

void
__ut_disagg_get_crypt_header(WT_ITEM *key_item, WT_CRYPT_HEADER **header)
{
    __disagg_get_crypt_header(key_item, header);
}

int
__ut_disagg_parse_version_and_check(
  WT_SESSION_IMPL *session, const WT_ITEM *meta_buf, WT_DISAGG_METADATA *metadata)
{
    return (__disagg_parse_version_and_check(session, meta_buf, metadata));
}
#endif
