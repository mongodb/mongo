/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_import_repair --
 *     Import a WiredTiger file into the database and reconstruct its metadata.
 */
int
__wt_import_repair(WT_SESSION_IMPL *session, const char *uri, char **configp)
{
    WT_BM *bm;
    WT_CKPT *ckpt, *ckptbase;
    WT_CONFIG_ITEM v;
    WT_DECL_ITEM(a);
    WT_DECL_ITEM(b);
    WT_DECL_ITEM(buf);
    WT_DECL_ITEM(checkpoint);
    WT_DECL_RET;
    WT_KEYED_ENCRYPTOR *kencryptor;
    uint32_t fileid;
    char *checkpoint_list, *config, *config_tmp, fileid_cfg[64], *metadata;
    const char *cfg[] = {WT_CONFIG_BASE(session, file_meta), NULL, NULL, NULL, NULL, NULL, NULL};
    bool shared;

    ckptbase = NULL;
    checkpoint_list = config = config_tmp = metadata = NULL;

    WT_ERR(__wt_scr_alloc(session, 0, &a));
    WT_ERR(__wt_scr_alloc(session, 0, &b));
    WT_ERR(__wt_scr_alloc(session, 1024, &buf));
    WT_ERR(__wt_scr_alloc(session, 0, &checkpoint));

    /*
     * Open the file, request block manager checkpoint information. We don't know the allocation
     * size, but 512B allows us to read the descriptor block and that's all we care about.
     */
    F_SET(session, WT_SESSION_IMPORT_REPAIR);
    WT_ERR(__wt_blkcache_open(session, uri, cfg, false, true, 512, NULL, &bm));
    ret = bm->checkpoint_last(bm, session, &metadata, &checkpoint_list, checkpoint);
    WT_TRET(bm->close(bm, session));
    F_CLR(session, WT_SESSION_IMPORT_REPAIR);
    WT_ERR(ret);

    /*
     * The metadata may have been encrypted, in which case it's also hexadecimal encoded. The
     * checkpoint included a boolean value set if the metadata was encrypted for easier failure
     * diagnosis.
     */
    WT_ERR(__wt_config_getones(session, metadata, "block_metadata_encrypted", &v));
    WT_ERR(__wt_btree_config_encryptor(session, cfg, &kencryptor));
    if ((kencryptor == NULL && v.val != 0) || (kencryptor != NULL && v.val == 0))
        WT_ERR_MSG(session, EINVAL,
          "%s: loaded object's encryption configuration doesn't match the database's encryption "
          "configuration",
          uri);
    /*
     * The metadata was quoted to avoid configuration string characters acting as separators.
     * Discard any quote characters.
     */
    WT_ERR(__wt_config_getones(session, metadata, "block_metadata", &v));
    if (v.len > 0 && (v.str[0] == '[' || v.str[0] == '(')) {
        ++v.str;
        v.len -= 2;
    }
    if (kencryptor == NULL) {
        WT_ERR(__wt_buf_grow(session, a, v.len + 1));
        WT_ERR(__wt_buf_set(session, a, v.str, v.len));
        ((uint8_t *)a->data)[a->size] = '\0';
    } else {
        WT_ERR(__wt_buf_grow(session, b, v.len));
        WT_ERR(__wt_nhex_to_raw(session, v.str, v.len, b));
        WT_ERR(__wt_buf_grow(session, a, b->size + 1));
        WT_ERR(__wt_decrypt(session, kencryptor->encryptor, 0, b, a));
        ((uint8_t *)a->data)[a->size] = '\0';
    }

    /*
     * OK, we've now got three chunks of data: the file's metadata from when the last checkpoint
     * started, the array of checkpoints as of when the last checkpoint was almost complete
     * (everything written but the avail list), and fixed-up checkpoint information from the last
     * checkpoint.
     *
     * Build and flatten the metadata and the checkpoint list, then insert it into the metadata for
     * this file.
     *
     * Reconstruct the incremental backup information, to indicate copying the whole file as an
     * imported file has not been part of backup. Strip out the checkpoint LSN, an imported file
     * isn't associated with any log files. Assign a unique file ID.
     */
    cfg[1] = a->data;
    cfg[2] = checkpoint_list;
    WT_ERR(__wt_reset_blkmod(session, a->data, buf));
    cfg[3] = buf->mem;
    cfg[4] = "checkpoint_lsn=";
    WT_WITH_SCHEMA_LOCK(session, fileid = WT_BTREE_ID_NAMESPACED(++S2C(session)->next_file_id));
    WT_ERR(__wt_snprintf(fileid_cfg, sizeof(fileid_cfg), "id=%" PRIu32, fileid));
    WT_ERR(ret);
    cfg[5] = fileid_cfg;
    WT_ERR(__wt_config_collapse(session, cfg, &config_tmp));

    /*
     * FIXME-WT-14723: import needs a little thought for shared tables once we've decided how to
     * allocate shared file IDs. It's not enough (even temporarily) to just share the allocated file
     * ID, since if we do that it may clash with another imported shared ID.
     */
    WT_ERR(__wt_btree_shared(session, uri, cfg, &shared));
    if (shared)
        WT_ERR_MSG(session, EINVAL, "TODO import of shared tree unsupported");

    /*
     * Now we need to retrieve the last checkpoint again but this time, with the correct allocation
     * size. When we did this earlier, we were able to read the descriptor block properly but the
     * checkpoint's byte representation was wrong because it was using the wrong allocation size.
     */
    WT_ERR(__wt_blkcache_open(session, uri, cfg, false, true, 0, NULL, &bm));
    __wt_free(session, checkpoint_list);
    __wt_free(session, metadata);
    ret = bm->checkpoint_last(bm, session, &metadata, &checkpoint_list, checkpoint);
    WT_TRET(bm->close(bm, session));

    /*
     * The metadata was correct as of immediately before the final checkpoint, but it's not quite
     * right. The block manager returned the corrected final checkpoint, put it all together.
     *
     * Get the checkpoint information from the file's metadata as an array of WT_CKPT structures.
     * Update the last checkpoint with the corrected information. Update the file's metadata with
     * the new checkpoint information.
     */
    WT_ERR(__wt_meta_ckptlist_get_from_config(session, false, &ckptbase, NULL, config_tmp));
    WT_CKPT_FOREACH (ckptbase, ckpt)
        if (ckpt->name == NULL || (ckpt + 1)->name == NULL)
            break;
    if (ckpt->name == NULL)
        WT_ERR_MSG(session, EINVAL, "no checkpoint information available to import");
    F_SET(ckpt, WT_CKPT_UPDATE);
    WT_ERR(__wt_buf_set(session, &ckpt->raw, checkpoint->data, checkpoint->size));
    WT_ERR(__wt_meta_ckptlist_update_config(session, ckptbase, config_tmp, &config));
    __wt_verbose_info(session, WT_VERB_CHECKPOINT, "import metadata: %s", config);
    *configp = config;
    WT_STAT_CONN_INCR(session, session_table_create_import_repair);
err:
    F_CLR(session, WT_SESSION_IMPORT_REPAIR);

    __wt_ckptlist_free(session, &ckptbase);

    __wt_free(session, checkpoint_list);
    if (ret != 0)
        __wt_free(session, config);
    __wt_free(session, config_tmp);
    __wt_free(session, metadata);

    __wt_scr_free(session, &a);
    __wt_scr_free(session, &b);
    __wt_scr_free(session, &buf);
    __wt_scr_free(session, &checkpoint);

    return (ret);
}
