/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __bmd_checkpoint_pack_raw --
 *     This function needs to do two things: Create a recovery point in the object store underlying
 *     this table and create an address cookie that is saved to the metadata (and used to find the
 *     checkpoint again).
 */
static int
__bmd_checkpoint_pack_raw(WT_BLOCK_DISAGG *block_disagg, WT_SESSION_IMPL *session,
  WT_ITEM *root_image, WT_PAGE_BLOCK_META *block_meta, WT_CKPT *ckpt)
{
    WT_BLOCK_DISAGG_ADDRESS_COOKIE root_cookie;
    uint32_t checksum, size;
    uint8_t *endp;

    WT_ASSERT(session, block_meta != NULL);
    WT_ASSERT(session, block_meta->page_id != WT_BLOCK_INVALID_PAGE_ID);

    /*
     * !!!
     * Our caller wants the final checkpoint size. Setting the size here violates layering,
     * but the alternative is a call for the btree layer to crack the checkpoint cookie into
     * its components, and that's a fair amount of work.
     */
    ckpt->size = block_meta->page_id;
    /* FIXME-WT-14610: What should be the checkpoint size? Do we need it? */

    /*
     * Write the root page out, and get back the address information for that page which will be
     * written into the block manager checkpoint cookie.
     */
    if (root_image == NULL) {
        ckpt->raw.data = NULL;
        ckpt->raw.size = 0;
    } else {
        /* Copy the checkpoint information into the checkpoint. */
        WT_RET(__wt_buf_init(session, &ckpt->raw, WT_BLOCK_CHECKPOINT_BUFFER));
        endp = ckpt->raw.mem;
        __wt_page_header_byteswap((void *)root_image->data);
        WT_RET(__wti_block_disagg_write_internal(
          session, block_disagg, root_image, block_meta, &size, &checksum, true, true));
        __wt_page_header_byteswap((void *)root_image->data);

        /* Initialize and pack the address cookie for the root page. */
        WT_CLEAR(root_cookie);
        root_cookie.page_id = block_meta->page_id;
        root_cookie.flags = 0;
        root_cookie.lsn = block_meta->disagg_lsn;
        root_cookie.base_lsn = block_meta->base_lsn;
        root_cookie.size = size;
        root_cookie.checksum = checksum;
        WT_RET(__wti_block_disagg_ckpt_pack(session, block_disagg, &endp, &root_cookie));

        ckpt->raw.size = WT_PTRDIFF(endp, ckpt->raw.mem);
        __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Checkpoint root page: root_id=%" PRIu64 " lsn=%" PRIu64 " base_lsn=%" PRIu64
          " root_size=%" PRIu32 " root_checksum=%" PRIx32,
          block_meta->page_id, block_meta->disagg_lsn, block_meta->base_lsn, size, checksum);
    }

    return (0);
}

/*
 * __wti_block_disagg_checkpoint --
 *     This function needs to do three things: Create a recovery point in the object store
 *     underlying this table and create an address cookie that is saved to the metadata (and used to
 *     find the checkpoint again) and save the content of the binary data added as a root page that
 *     can be retrieved to start finding content for the tree.
 */
int
__wti_block_disagg_checkpoint(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *root_image,
  WT_PAGE_BLOCK_META *block_meta, WT_CKPT *ckptbase, bool data_checksum)
{
    WT_BLOCK_DISAGG *block_disagg;
    WT_CKPT *ckpt;

    WT_UNUSED(data_checksum);

    block_disagg = (WT_BLOCK_DISAGG *)bm->block;

    /*
     * Generate a checkpoint cookie used to find the checkpoint again (and distinguish it from a
     * fake checkpoint).
     */
    WT_CKPT_FOREACH (ckptbase, ckpt)
        if (F_ISSET(ckpt, WT_CKPT_ADD))
            WT_RET(__bmd_checkpoint_pack_raw(block_disagg, session, root_image, block_meta, ckpt));

    return (0);
}

/*
 * __wti_block_disagg_checkpoint_resolve --
 *     Resolve the checkpoint.
 */
int
__wti_block_disagg_checkpoint_resolve(WT_BM *bm, WT_SESSION_IMPL *session, bool failed)
{
    WT_BLOCK_DISAGG *block_disagg;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *md_cursor;
    WT_DECL_RET;
    size_t len;
    uint64_t checkpoint_timestamp;
    char *md_key;
    const char *md_value;

    block_disagg = (WT_BLOCK_DISAGG *)bm->block;
    conn = S2C(session);

    md_cursor = NULL;
    md_key = NULL;

    if (failed)
        return (0);

    /* Allocate a buffer for metadata keys. */
    len = strlen("file:") + strlen(block_disagg->name) + 4;
    WT_ERR(__wt_calloc_def(session, len, &md_key));

    /* Get a metadata cursor pointing to this table */
    WT_ERR(__wt_metadata_cursor(session, &md_cursor));
    WT_ERR(__wt_snprintf(md_key, len, "file:%s", block_disagg->name));
    md_cursor->set_key(md_cursor, md_key);
    WT_ERR(md_cursor->search(md_cursor));
    WT_ERR(md_cursor->get_value(md_cursor, &md_value));

    /*
     * Release the metadata cursor early, so that the subsequent functions can reuse the cached
     * metadata cursor in the session.
     */
    WT_ERR(__wt_metadata_cursor_release(session, &md_cursor));

    /*
     * Store the metadata of regular shared tables in the shared metadata table. Store the metadata
     * of the shared metadata table in the system-level metadata (similar to the turtle file).
     */
    if (strcmp(block_disagg->name, WT_DISAGG_METADATA_FILE) == 0) {
        /* Get the config we want to print to the metadata file */
        WT_ERR(__wt_config_getones(session, md_value, "checkpoint", &cval));
        checkpoint_timestamp = conn->disaggregated_storage.cur_checkpoint_timestamp;
        WT_ERR(__wt_disagg_put_checkpoint_meta(session, cval.str, cval.len, checkpoint_timestamp));
    } else {
        /* Keep all metadata for regular tables. */
        WT_SAVE_DHANDLE(
          session, ret = __wt_disagg_update_shared_metadata(session, md_key, md_value));
        WT_ERR(ret);

        /* Check if we need to include any other metadata keys. */
        if (WT_SUFFIX_MATCH(block_disagg->name, ".wt")) {
            WT_ERR(__wt_snprintf(md_key, len, "%s", block_disagg->name));
            md_key[strlen(md_key) - 3] = '\0'; /* Remove the .wt suffix */
            WT_ERR_NOTFOUND_OK(__wt_disagg_copy_shared_metadata_layered(session, md_key), false);
        }

        /* Check if we need to include any other metadata keys for layered tables. */
        if (WT_SUFFIX_MATCH(block_disagg->name, ".wt_stable")) {
            WT_ERR(__wt_snprintf(md_key, len, "%s", block_disagg->name));
            md_key[strlen(md_key) - 10] = '\0'; /* Remove the .wt_stable suffix */
            WT_ERR_NOTFOUND_OK(__wt_disagg_copy_shared_metadata_layered(session, md_key), false);
        }
    }

err:
    __wt_free(session, md_key);
    if (md_cursor != NULL)
        WT_TRET(__wt_metadata_cursor_release(session, &md_cursor));

    return (ret);
}

/*
 * __wti_block_disagg_checkpoint_load --
 *     Load a checkpoint. This involves (1) cracking the checkpoint cookie open (2) loading the root
 *     page from the object store, (3) re-packing the root page's address cookie into root_addr.
 */
int
__wti_block_disagg_checkpoint_load(WT_BM *bm, WT_SESSION_IMPL *session, const uint8_t *addr,
  size_t addr_size, uint8_t *root_addr, size_t *root_addr_sizep, bool checkpoint)
{
    WT_BLOCK_DISAGG *block_disagg;
    WT_BLOCK_DISAGG_ADDRESS_COOKIE root_cookie;
    uint8_t *endp;

    WT_UNUSED(session);
    WT_UNUSED(addr_size);
    WT_UNUSED(checkpoint);

    block_disagg = (WT_BLOCK_DISAGG *)bm->block;

    *root_addr_sizep = 0;

    if (addr == NULL || addr_size == 0)
        return (0);

    WT_RET(__wti_block_disagg_ckpt_unpack(session, block_disagg, addr, addr_size, &root_cookie));

    /*
     * Read root page address.
     */
    endp = root_addr;
    WT_RET(__wti_block_disagg_addr_pack(session, &endp, &root_cookie));
    *root_addr_sizep = WT_PTRDIFF(endp, root_addr);

    __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Loading checkpoint: root_id=%" PRIu64 " flags=%" PRIx64 " lsn=%" PRIu64 " base_lsn=%" PRIu64
      " root_size=%" PRIu32 " root_checksum=%" PRIx32,
      root_cookie.page_id, root_cookie.flags, root_cookie.lsn, root_cookie.base_lsn,
      root_cookie.size, root_cookie.checksum);

    return (0);
}
