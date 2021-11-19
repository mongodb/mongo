/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __ckpt_last(WT_SESSION_IMPL *, const char *, WT_CKPT *);
static int __ckpt_last_name(WT_SESSION_IMPL *, const char *, const char **);
static int __ckpt_load(WT_SESSION_IMPL *, WT_CONFIG_ITEM *, WT_CONFIG_ITEM *, WT_CKPT *);
static int __ckpt_named(WT_SESSION_IMPL *, const char *, const char *, WT_CKPT *);
static int __ckpt_set(WT_SESSION_IMPL *, const char *, const char *, bool);
static int __ckpt_version_chk(WT_SESSION_IMPL *, const char *, const char *);
static int __meta_blk_mods_load(WT_SESSION_IMPL *, const char *, WT_CKPT *, WT_CKPT *, bool);
/*
 * __ckpt_load_blk_mods --
 *     Load the block information from the config string.
 */
static int
__ckpt_load_blk_mods(WT_SESSION_IMPL *session, const char *config, WT_CKPT *ckpt)
{
    WT_BLKINCR *blkincr;
    WT_BLOCK_MODS *blk_mod;
    WT_CONFIG blkconf;
    WT_CONFIG_ITEM b, k, v;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint64_t i;

    conn = S2C(session);
    if (config == NULL)
        return (0);
    /*
     * We could be reading in a configuration from an earlier release. If the string doesn't exist
     * then we're done.
     */
    if ((ret = __wt_config_getones(session, config, "checkpoint_backup_info", &v)) != 0)
        return (ret == WT_NOTFOUND ? 0 : ret);
    __wt_config_subinit(session, &blkconf, &v);
    /*
     * Load block lists. Ignore any that have an id string that is not known.
     *
     * Remove those not known (TODO).
     */
    blkincr = NULL;
    while ((ret = __wt_config_next(&blkconf, &k, &v)) == 0) {
        /*
         * See if this is a valid backup string.
         */
        for (i = 0; i < WT_BLKINCR_MAX; ++i) {
            blkincr = &conn->incr_backups[i];
            if (blkincr->id_str != NULL && WT_STRING_MATCH(blkincr->id_str, k.str, k.len))
                break;
        }
        if (i == WT_BLKINCR_MAX)
            /*
             * This is the place to note that we want to remove an unknown id.
             */
            continue;

        /*
         * We have a valid entry. Load the block information.
         */
        blk_mod = &ckpt->backup_blocks[i];
        WT_RET(__wt_strdup(session, blkincr->id_str, &blk_mod->id_str));
        WT_RET(__wt_config_subgets(session, &v, "granularity", &b));
        blk_mod->granularity = (uint64_t)b.val;
        WT_RET(__wt_config_subgets(session, &v, "nbits", &b));
        blk_mod->nbits = (uint64_t)b.val;
        WT_RET(__wt_config_subgets(session, &v, "offset", &b));
        blk_mod->offset = (uint64_t)b.val;
        /*
         * The rename configuration string component was added later. So don't error if we don't
         * find it in the string. If we don't have it, we're not doing a rename.
         */
        ret = __wt_config_subgets(session, &v, "rename", &b);
        WT_RET_NOTFOUND_OK(ret);
        if (ret == 0 && b.val)
            F_SET(blk_mod, WT_BLOCK_MODS_RENAME);
        else
            F_CLR(blk_mod, WT_BLOCK_MODS_RENAME);
        ret = __wt_config_subgets(session, &v, "blocks", &b);
        WT_RET_NOTFOUND_OK(ret);
        if (ret != WT_NOTFOUND) {
            WT_RET(__wt_backup_load_incr(session, &b, &blk_mod->bitstring, blk_mod->nbits));
            F_SET(blk_mod, WT_BLOCK_MODS_VALID);
        }
    }
    return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __wt_meta_checkpoint --
 *     Return a file's checkpoint information.
 */
int
__wt_meta_checkpoint(
  WT_SESSION_IMPL *session, const char *fname, const char *checkpoint, WT_CKPT *ckpt)
{
    WT_DECL_RET;
    char *config;

    config = NULL;

    /* Clear the returned information. */
    memset(ckpt, 0, sizeof(*ckpt));

    /* Retrieve the metadata entry for the file. */
    WT_ERR(__wt_metadata_search(session, fname, &config));

/*
 * Check for the version mismatch in wiredtiger standalone build when compatibility has not been
 * configured.
 */
#ifdef WT_STANDALONE_BUILD
    if (!F_ISSET(S2C(session), WT_CONN_COMPATIBILITY))
        /* Check the major/minor version numbers. */
        WT_ERR(__ckpt_version_chk(session, fname, config));
#else
    /* Check the major/minor version numbers. */
    WT_ERR(__ckpt_version_chk(session, fname, config));
#endif

    /*
     * Retrieve the named checkpoint or the last checkpoint.
     *
     * If we don't find a named checkpoint, we're done, they're read-only. If we don't find a
     * default checkpoint, it's creation, return "no data" and let our caller handle it.
     */
    if (checkpoint == NULL) {
        if ((ret = __ckpt_last(session, config, ckpt)) == WT_NOTFOUND) {
            ret = 0;
            ckpt->addr.data = ckpt->raw.data = NULL;
            ckpt->addr.size = ckpt->raw.size = 0;
        }
    } else
        WT_ERR(__ckpt_named(session, checkpoint, config, ckpt));

err:
    __wt_free(session, config);
    return (ret);
}

/*
 * __wt_meta_checkpoint_last_name --
 *     Return the last unnamed checkpoint's name.
 */
int
__wt_meta_checkpoint_last_name(WT_SESSION_IMPL *session, const char *fname, const char **namep)
{
    WT_DECL_RET;
    char *config;

    config = NULL;

    /* Retrieve the metadata entry for the file. */
    WT_ERR(__wt_metadata_search(session, fname, &config));

    /* Check the major/minor version numbers. */
    WT_ERR(__ckpt_version_chk(session, fname, config));

    /* Retrieve the name of the last unnamed checkpoint. */
    WT_ERR(__ckpt_last_name(session, config, namep));

err:
    __wt_free(session, config);
    return (ret);
}

/*
 * __wt_meta_checkpoint_clear --
 *     Clear a file's checkpoint.
 */
int
__wt_meta_checkpoint_clear(WT_SESSION_IMPL *session, const char *fname)
{
    /*
     * If we are unrolling a failed create, we may have already removed the metadata entry. If no
     * entry is found to update and we're trying to clear the checkpoint, just ignore it.
     */
    WT_RET_NOTFOUND_OK(__ckpt_set(session, fname, NULL, false));

    return (0);
}

/*
 * __ckpt_set --
 *     Set a file's checkpoint.
 */
static int
__ckpt_set(WT_SESSION_IMPL *session, const char *fname, const char *v, bool use_base)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    size_t meta_base_length;
    char *config, *newcfg;
    const char *cfg[3], *meta_base, *str;

    /*
     * If the caller knows we're on a path like checkpoints where we have a valid checkpoint and
     * checkpoint LSN and should use the base, then use that faster path. Some paths don't have a
     * dhandle or want to have the older value retained from the existing metadata. In those cases,
     * use the slower path through configuration parsing functions.
     */
    config = newcfg = NULL;
    dhandle = session->dhandle;
    str = v == NULL ? "checkpoint=(),checkpoint_backup_info=(),checkpoint_lsn=" : v;
    if (use_base && dhandle != NULL) {
        WT_ERR(__wt_scr_alloc(session, 0, &tmp));
        WT_ASSERT(session, strcmp(dhandle->name, fname) == 0);

        /* Check the metadata is not corrupted. */
        meta_base = dhandle->meta_base;
        meta_base_length = strlen(meta_base);
        if (dhandle->meta_base_length != meta_base_length)
            WT_ERR_PANIC(session, WT_PANIC,
              "Corrupted metadata. The original metadata length was %lu while the new one is %lu.",
              dhandle->meta_base_length, meta_base_length);
#ifdef HAVE_DIAGNOSTIC
        if (!WT_STREQ(dhandle->orig_meta_base, meta_base))
            WT_ERR_PANIC(session, WT_PANIC,
              "Corrupted metadata. The original metadata length was %lu while the new one is %lu. "
              "The original metadata inserted was %s and the current "
              "metadata is now %s.",
              dhandle->meta_base_length, meta_base_length, dhandle->orig_meta_base, meta_base);
#endif

        /* Concatenate the metadata base string with the checkpoint string. */
        WT_ERR(__wt_buf_fmt(session, tmp, "%s,%s", meta_base, str));
        /*
         * Check the new metadata length is at least as long as the original metadata string with
         * the checkpoint base stripped out.
         */
        WT_ASSERT(session, tmp->size >= dhandle->meta_base_length);
        WT_ERR(__wt_metadata_update(session, fname, tmp->mem));
    } else {
        /* Retrieve the metadata for this file. */
        WT_ERR(__wt_metadata_search(session, fname, &config));
        /* Replace the checkpoint entry. */
        cfg[0] = config;
        cfg[1] = str;
        cfg[2] = NULL;
        WT_ERR(__wt_config_collapse(session, cfg, &newcfg));
        WT_ERR(__wt_metadata_update(session, fname, newcfg));
    }

err:
    __wt_scr_free(session, &tmp);
    __wt_free(session, config);
    __wt_free(session, newcfg);
    return (ret);
}

/*
 * __ckpt_named --
 *     Return the information associated with a file's named checkpoint.
 */
static int
__ckpt_named(WT_SESSION_IMPL *session, const char *checkpoint, const char *config, WT_CKPT *ckpt)
{
    WT_CONFIG ckptconf;
    WT_CONFIG_ITEM k, v;

    WT_RET(__wt_config_getones(session, config, "checkpoint", &v));
    __wt_config_subinit(session, &ckptconf, &v);

    /*
     * Take the first match: there should never be more than a single checkpoint of any name.
     */
    while (__wt_config_next(&ckptconf, &k, &v) == 0)
        if (WT_STRING_MATCH(checkpoint, k.str, k.len))
            return (__ckpt_load(session, &k, &v, ckpt));

    return (WT_NOTFOUND);
}

/*
 * __ckpt_last --
 *     Return the information associated with the file's last checkpoint.
 */
static int
__ckpt_last(WT_SESSION_IMPL *session, const char *config, WT_CKPT *ckpt)
{
    WT_CONFIG ckptconf;
    WT_CONFIG_ITEM a, k, v;
    int64_t found;

    WT_RET(__wt_config_getones(session, config, "checkpoint", &v));
    __wt_config_subinit(session, &ckptconf, &v);
    for (found = 0; __wt_config_next(&ckptconf, &k, &v) == 0;) {
        /* Ignore checkpoints before the ones we've already seen. */
        WT_RET(__wt_config_subgets(session, &v, "order", &a));
        if (found) {
            if (a.val < found)
                continue;
            __wt_meta_checkpoint_free(session, ckpt);
        }
        found = a.val;
        WT_RET(__ckpt_load(session, &k, &v, ckpt));
    }

    return (found ? 0 : WT_NOTFOUND);
}

/*
 * __ckpt_last_name --
 *     Return the name associated with the file's last unnamed checkpoint.
 */
static int
__ckpt_last_name(WT_SESSION_IMPL *session, const char *config, const char **namep)
{
    WT_CONFIG ckptconf;
    WT_CONFIG_ITEM a, k, v;
    WT_DECL_RET;
    int64_t found;

    *namep = NULL;

    WT_ERR(__wt_config_getones(session, config, "checkpoint", &v));
    __wt_config_subinit(session, &ckptconf, &v);
    for (found = 0; __wt_config_next(&ckptconf, &k, &v) == 0;) {
        /*
         * We only care about unnamed checkpoints; applications may not use any matching prefix as a
         * checkpoint name, the comparison is pretty simple.
         */
        if (k.len < strlen(WT_CHECKPOINT) ||
          strncmp(k.str, WT_CHECKPOINT, strlen(WT_CHECKPOINT)) != 0)
            continue;

        /* Ignore checkpoints before the ones we've already seen. */
        WT_ERR(__wt_config_subgets(session, &v, "order", &a));
        if (found && a.val < found)
            continue;

        __wt_free(session, *namep);
        WT_ERR(__wt_strndup(session, k.str, k.len, namep));
        found = a.val;
    }
    if (!found)
        ret = WT_NOTFOUND;

    if (0) {
err:
        __wt_free(session, *namep);
    }
    return (ret);
}

/*
 * __wt_meta_block_metadata --
 *     Build a version of the file's metadata for the block manager to store.
 */
int
__wt_meta_block_metadata(WT_SESSION_IMPL *session, const char *config, WT_CKPT *ckpt)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_ITEM(a);
    WT_DECL_ITEM(b);
    WT_DECL_RET;
    WT_KEYED_ENCRYPTOR *kencryptor;
    size_t encrypt_size, metadata_len;
    const char *metadata, *filecfg[] = {WT_CONFIG_BASE(session, file_meta), NULL, NULL};

    WT_ERR(__wt_scr_alloc(session, 0, &a));
    WT_ERR(__wt_scr_alloc(session, 0, &b));

    /* Fill out the configuration array for normal retrieval. */
    filecfg[1] = config;

    /*
     * Find out if this file is encrypted. If encrypting, encrypt and encode. The metadata has to be
     * encrypted because it contains private data (for example, column names). We pass the block
     * manager text that describes the metadata (the encryption information), and the possibly
     * encrypted metadata encoded as a hexadecimal string.
     */
    WT_ERR(__wt_btree_config_encryptor(session, filecfg, &kencryptor));
    if (kencryptor == NULL) {
        metadata = config;
        metadata_len = strlen(config);
    } else {
        WT_ERR(__wt_buf_set(session, a, config, strlen(config)));
        __wt_encrypt_size(session, kencryptor, a->size, &encrypt_size);
        WT_ERR(__wt_buf_grow(session, b, encrypt_size));
        WT_ERR(__wt_encrypt(session, kencryptor, 0, a, b));
        WT_ERR(__wt_buf_grow(session, a, b->size * 2 + 1));
        __wt_fill_hex(b->mem, b->size, a->mem, a->memsize, &a->size);

        metadata = a->data;
        metadata_len = a->size;
    }

    /*
     * Get a copy of the encryption information and flag if we're doing encryption. The latter isn't
     * necessary, but it makes it easier to diagnose issues during the load.
     */
    WT_ERR(__wt_config_gets(session, filecfg, "encryption", &cval));
    WT_ERR(__wt_buf_fmt(session, b,
      "encryption=%.*s,block_metadata_encrypted=%s,block_metadata=[%.*s]", (int)cval.len, cval.str,
      kencryptor == NULL ? "false" : "true", (int)metadata_len, metadata));
    WT_ERR(__wt_strndup(session, b->data, b->size, &ckpt->block_metadata));

err:
    __wt_scr_free(session, &a);
    __wt_scr_free(session, &b);
    return (ret);
}

/*
 * __ckpt_compare_order --
 *     Qsort comparison routine for the checkpoint list.
 */
static int WT_CDECL
__ckpt_compare_order(const void *a, const void *b)
{
    WT_CKPT *ackpt, *bckpt;

    ackpt = (WT_CKPT *)a;
    bckpt = (WT_CKPT *)b;

    return (ackpt->order > bckpt->order ? 1 : -1);
}

/*
 * __ckpt_valid_blk_mods --
 *     Make sure that this set of block mods reflects the current valid backup identifiers. If so,
 *     there is nothing to do. If not, free up old information and set it up for the current
 *     information.
 */
static int
__ckpt_valid_blk_mods(WT_SESSION_IMPL *session, WT_CKPT *ckpt, bool rename)
{
    WT_BLKINCR *blk;
    WT_BLOCK_MODS *blk_mod;
    uint64_t i;
    bool free, setup;

    WT_ASSERT(session, F_ISSET(ckpt, WT_CKPT_ADD));
    for (i = 0; i < WT_BLKINCR_MAX; ++i) {
        blk = &S2C(session)->incr_backups[i];
        blk_mod = &ckpt->backup_blocks[i];

        /*
         * Check the state of our block list array compared to the global one. There are
         * several possibilities:
         * - There is no global information for this index, nothing to do but free our resources.
         * - We don't have any backup information locally. Set up our entry.
         * - Our entry's id string matches the current global information. We just want to add our
         *   information to the existing list.
         * - Our entry's id string does not match the current one. It is outdated. Free old
         * resources and then set up our entry.
         */

        /* Check if the global entry is valid at our index. */
        if (!F_ISSET(blk, WT_BLKINCR_VALID)) {
            free = true;
            setup = false;
        } else if (F_ISSET(blk_mod, WT_BLOCK_MODS_VALID) &&
          WT_STRING_MATCH(blk_mod->id_str, blk->id_str, strlen(blk->id_str))) {
            /* We match, keep our entry and don't set up. */
            setup = false;
            free = false;
        } else {
            /* We don't match, free any old information. */
            free = true;
            setup = true;
        }

        /* If we are keeping or setting up an entry on a rename, set the flag. */
        if (rename && (!free || setup))
            F_SET(blk_mod, WT_BLOCK_MODS_RENAME);

        /* Free any old information if we need to do so. */
        if (free && F_ISSET(blk_mod, WT_BLOCK_MODS_VALID)) {
            __wt_free(session, blk_mod->id_str);
            __wt_buf_free(session, &blk_mod->bitstring);
            blk_mod->nbits = 0;
            blk_mod->granularity = 0;
            blk_mod->offset = 0;
            F_CLR(blk_mod, WT_BLOCK_MODS_VALID);
        }

        /* Set up the block list to point to the current information. */
        if (setup) {
            WT_RET(__wt_strdup(session, blk->id_str, &blk_mod->id_str));
            WT_CLEAR(blk_mod->bitstring);
            blk_mod->granularity = S2C(session)->incr_granularity;
            blk_mod->nbits = 0;
            blk_mod->offset = 0;
            F_SET(blk_mod, WT_BLOCK_MODS_VALID);
        }
    }
    return (0);
}

/*
 * __ckpt_copy_blk_mods --
 *     Copy the block mods from a source checkpoint to the destination checkpoint.
 */
static int
__ckpt_copy_blk_mods(WT_SESSION_IMPL *session, WT_CKPT *src_ckpt, WT_CKPT *dst_ckpt)
{
    uint64_t i;

    for (i = 0; i < WT_BLKINCR_MAX; ++i) {
        WT_RET(__wt_strdup(
          session, src_ckpt->backup_blocks[i].id_str, &dst_ckpt->backup_blocks[i].id_str));
        WT_RET(__wt_buf_set(session, &dst_ckpt->backup_blocks[i].bitstring,
          src_ckpt->backup_blocks[i].bitstring.data, src_ckpt->backup_blocks[i].bitstring.size));
        dst_ckpt->backup_blocks[i].nbits = src_ckpt->backup_blocks[i].nbits;
        dst_ckpt->backup_blocks[i].offset = src_ckpt->backup_blocks[i].offset;
        dst_ckpt->backup_blocks[i].granularity = src_ckpt->backup_blocks[i].granularity;
        dst_ckpt->backup_blocks[i].flags = src_ckpt->backup_blocks[i].flags;
    }

    return (0);
}

/*
 * __meta_blk_mods_load --
 *     Load the block mods for a given checkpoint and set up all the information to store. Load from
 *     either the metadata or from a base checkpoint.
 */
static int
__meta_blk_mods_load(
  WT_SESSION_IMPL *session, const char *config, WT_CKPT *base_ckpt, WT_CKPT *ckpt, bool rename)
{
    /*
     * Load most recent checkpoint backup blocks to this checkpoint, either from metadata or from a
     * previous checkpoint.
     */
    if (config != NULL) {
        /* Load from metadata. */
        WT_RET(__ckpt_load_blk_mods(session, config, ckpt));
        WT_RET(__wt_meta_block_metadata(session, config, ckpt));
    } else {
        /* Load from an existing base checkpoint. */
        WT_ASSERT(session, base_ckpt != NULL);
        WT_RET(__ckpt_copy_blk_mods(session, base_ckpt, ckpt));
        WT_RET(__wt_strndup(session, base_ckpt->block_metadata, strlen(base_ckpt->block_metadata),
          &ckpt->block_metadata));
    }

    /*
     * Set the add-a-checkpoint flag, and if we're doing incremental backups, request a list of the
     * checkpoint's modified blocks from the block manager.
     */
    F_SET(ckpt, WT_CKPT_ADD);
    if (F_ISSET(S2C(session), WT_CONN_INCR_BACKUP)) {
        F_SET(ckpt, WT_CKPT_BLOCK_MODS);
        WT_RET(__ckpt_valid_blk_mods(session, ckpt, rename));
    }
    return (0);
}

/*
 * __meta_ckptlist_allocate_new_ckpt --
 *     Provided a checkpoint list, allocate a new checkpoint. Either use the last checkpoint in the
 *     list or the file metadata to initialize this new checkpoint.
 */
static int
__meta_ckptlist_allocate_new_ckpt(
  WT_SESSION_IMPL *session, WT_CKPT **ckptbasep, size_t *allocated, const char *config)
{
    WT_CKPT *ckptbase, *ckpt;
    WT_CONNECTION_IMPL *conn;
    size_t slot;
    uint64_t most_recent;

    ckptbase = *ckptbasep;
    conn = S2C(session);
    slot = 0;

    if (ckptbase != NULL)
        WT_CKPT_FOREACH (ckptbase, ckpt)
            slot++;

    /*
     * Either we have a configuration or an existing checkpoint to initialize with. Also, If we are
     * using an existing checkpoint, we must have the associated metadata.
     */
    WT_ASSERT(session, config != NULL || (slot != 0 && ckptbase[slot - 1].block_metadata != NULL));

    /*
     * This isn't clean, but there's necessary cooperation between the schema layer (that maintains
     * the list of checkpoints), the btree layer (that knows when the root page is written, creating
     * a new checkpoint), and the block manager (which actually creates the checkpoint). All of that
     * cooperation is handled in the array of checkpoint structures referenced from the WT_BTREE
     * structure.
     *
     * Allocate a slot for a new value, plus a slot to mark the end.
     */
    WT_RET(__wt_realloc_def(session, allocated, slot + 2, &ckptbase));
    *ckptbasep = ckptbase;

    ckpt = &ckptbase[slot];
    ckpt->order = (slot == 0) ? 1 : ckptbase[slot - 1].order + 1;
    __wt_seconds(session, &ckpt->sec);
    /*
     * Update time value for most recent checkpoint, not letting it move backwards. It is possible
     * to race here, so use atomic CAS. This code relies on the fact that anyone we race with will
     * only increase (never decrease) the most recent checkpoint time value.
     *
     * Update the time for this checkpoint not letting it move backwards. If tiered storage is in
     * use move it at least up to the most recent flush. Then move it up to at least the most
     * checkpoint.
     *
     * NOTE: reading the most recent flush time is not an ordered read because currently checkpoint
     * and flush tier are mutually exclusive.
     */
    for (;;) {
        WT_ORDERED_READ(most_recent, conn->ckpt_most_recent);
        ckpt->sec = WT_MAX(ckpt->sec, conn->flush_most_recent);
        ckpt->sec = WT_MAX(ckpt->sec, most_recent);
        if (ckpt->sec == most_recent ||
          __wt_atomic_cas64(&conn->ckpt_most_recent, most_recent, ckpt->sec))
            break;
    }

    /* Either load block mods from the config, or from the previous checkpoint. */
    WT_RET(
      __meta_blk_mods_load(session, config, (slot == 0 ? NULL : &ckptbase[slot - 1]), ckpt, false));
    WT_ASSERT(session, ckpt->block_metadata != NULL);

    return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __assert_ckpt_matches --
 *     Assert that given two checkpoints match.
 */
static void
__assert_ckpt_matches(WT_SESSION_IMPL *session, WT_CKPT *ckpt_a, WT_CKPT *ckpt_b)
{
    /*
     * We are not checking checkpoint time, because there could be a minute difference depending
     * upon when the checkpoint information was generated. This is acceptable.
     */
    WT_ASSERT(session,
      (ckpt_a->name == NULL && ckpt_b->name == NULL) ||
        (ckpt_a->name != NULL && ckpt_b->name != NULL && strcmp(ckpt_a->name, ckpt_b->name) == 0));
    WT_ASSERT(session, ckpt_a->order == ckpt_b->order);
    WT_ASSERT(session, ckpt_a->size == ckpt_b->size);
    WT_ASSERT(session, ckpt_a->write_gen == ckpt_b->write_gen);
    WT_ASSERT(session, ckpt_a->run_write_gen == ckpt_b->run_write_gen);
    WT_ASSERT(session,
      ckpt_a->ta.newest_start_durable_ts == ckpt_b->ta.newest_start_durable_ts &&
        ckpt_a->ta.newest_stop_durable_ts == ckpt_b->ta.newest_stop_durable_ts &&
        ckpt_a->ta.oldest_start_ts == ckpt_b->ta.oldest_start_ts &&
        ckpt_a->ta.newest_txn == ckpt_b->ta.newest_txn &&
        ckpt_a->ta.newest_stop_ts == ckpt_b->ta.newest_stop_ts &&
        ckpt_a->ta.newest_stop_txn == ckpt_b->ta.newest_stop_txn &&
        ckpt_a->ta.prepare == ckpt_b->ta.prepare);
    /*
     * The two WT_CKPT structures are created through different paths, specifically in one path the
     * WT_CKPT.addr and WT_CKPT.raw fields are taken from a configuration file as strings including
     * a training nul byte. Use the minimum size of the data to ignore that nul byte. Passing nul
     * pointers to memcmp is undefined, so handle that separately.
     */
    WT_ASSERT(session,
      (ckpt_a->addr.data == NULL && ckpt_b->addr.data == NULL) ||
        (ckpt_a->addr.data != NULL && ckpt_b->addr.data != NULL &&
          memcmp(ckpt_a->addr.data, ckpt_b->addr.data,
            WT_MIN(ckpt_a->addr.size, ckpt_b->addr.size)) == 0));
    WT_ASSERT(session,
      (ckpt_a->raw.data == NULL && ckpt_b->raw.data == NULL) ||
        (ckpt_a->raw.data != NULL && ckpt_b->raw.data != NULL &&
          memcmp(ckpt_a->raw.data, ckpt_b->raw.data, WT_MIN(ckpt_a->raw.size, ckpt_b->raw.size)) ==
            0));
    WT_ASSERT(session, ckpt_a->bpriv == NULL && ckpt_b->bpriv == NULL);
    WT_ASSERT(session, ckpt_a->flags == ckpt_b->flags);
}

/*
 * __assert_checkpoint_list_matches --
 *     Assert that two given checkpoint lists match.
 */
static void
__assert_checkpoint_list_matches(WT_SESSION_IMPL *session, WT_CKPT *saved_list, WT_CKPT *new_list)
{
    WT_CKPT *ckpt_saved, *ckpt_new;

    for (ckpt_saved = saved_list, ckpt_new = new_list;
         ckpt_saved != NULL && ckpt_saved->order != 0 && ckpt_new != NULL && ckpt_new->order != 0;
         ckpt_saved++, ckpt_new++)
        __assert_ckpt_matches(session, ckpt_saved, ckpt_new);

    WT_ASSERT(session,
      (ckpt_saved == NULL && ckpt_new == NULL) ||
        ((ckpt_saved != NULL && ckpt_saved->order == 0) &&
          (ckpt_new != NULL && ckpt_new->order == 0)));
}
#endif

/*
 * __wt_meta_ckptlist_get --
 *     Load all available checkpoint information for a file. Either use a cached copy of the
 *     checkpoints or rebuild from the metadata.
 */
int
__wt_meta_ckptlist_get(
  WT_SESSION_IMPL *session, const char *fname, bool update, WT_CKPT **ckptbasep, size_t *allocated)
{
    WT_BTREE *btree;
#ifdef HAVE_DIAGNOSTIC
    WT_CKPT *ckptbase_comp;
#endif
    WT_DECL_RET;
    char *config;

    *ckptbasep = NULL;

    if (allocated != NULL)
        *allocated = 0;

    btree = S2BT(session);
    config = NULL;

    /*
     * Get the list of checkpoints for this file: We try to cache the ckptlist between each rebuild
     * from the metadata. But there might not be one, as there are operations that can invalidate a
     * ckptlist. So, use a cached ckptlist if there is one. Otherwise go through slow path of
     * re-generating the ckptlist by reading the metadata. Also, we avoid using a cached checkpoint
     * list for metadata itself.
     */
    if (!WT_IS_METADATA(session->dhandle) && btree->ckpt != NULL) {
        *ckptbasep = btree->ckpt;
        if (update)
            WT_ERR(__meta_ckptlist_allocate_new_ckpt(
              session, ckptbasep, &btree->ckpt_bytes_allocated, NULL));
        if (allocated != NULL)
            *allocated = btree->ckpt_bytes_allocated;
#ifdef HAVE_DIAGNOSTIC
        /*
         * Sanity check: Let's compare to a list generated from metadata. There should be no
         * differences.
         */
        WT_ERR(__wt_metadata_search(session, fname, &config));
        if ((ret = __wt_meta_ckptlist_get_from_config(
               session, update, &ckptbase_comp, NULL, config)) == 0)
            __assert_checkpoint_list_matches(session, *ckptbasep, ckptbase_comp);
        __wt_meta_ckptlist_free(session, &ckptbase_comp);
        WT_ERR(ret);
#endif
    } else {
        WT_ERR(__wt_metadata_search(session, fname, &config));
        WT_ERR(__wt_meta_ckptlist_get_from_config(session, update, ckptbasep, allocated, config));
    }

err:
    __wt_free(session, config);
    return (ret);
}

/*
 * __wt_meta_ckptlist_get_from_config --
 *     Provided a metadata config, load all available checkpoint information for a file.
 */
int
__wt_meta_ckptlist_get_from_config(WT_SESSION_IMPL *session, bool update, WT_CKPT **ckptbasep,
  size_t *allocatedp, const char *config)
{
    WT_CKPT *ckpt, *ckptbase;
    WT_CONFIG ckptconf;
    WT_CONFIG_ITEM k, v;
    WT_DECL_RET;
    size_t allocated, slot;

    *ckptbasep = NULL;
    if (allocatedp != NULL)
        *allocatedp = 0;

    ckptbase = NULL;
    allocated = slot = 0;

    /* Load any existing checkpoints into the array. */
    if ((ret = __wt_config_getones(session, config, "checkpoint", &v)) == 0) {
        __wt_config_subinit(session, &ckptconf, &v);
        for (; __wt_config_next(&ckptconf, &k, &v) == 0; ++slot) {
            /*
             * Allocate a slot for a new value, plus a slot to mark the end.
             */
            WT_ERR(__wt_realloc_def(session, &allocated, slot + 2, &ckptbase));
            ckpt = &ckptbase[slot];

            WT_ERR(__ckpt_load(session, &k, &v, ckpt));
            WT_ERR(__wt_meta_block_metadata(session, config, ckpt));
        }
    }
    WT_ERR_NOTFOUND_OK(ret, false);
    if (!update && slot == 0)
        WT_ERR(WT_NOTFOUND);

    /* Sort in creation-order. */
    __wt_qsort(ckptbase, slot, sizeof(WT_CKPT), __ckpt_compare_order);

    /* The caller might be asking for a new checkpoint to be allocated. */
    if (update)
        WT_ERR(__meta_ckptlist_allocate_new_ckpt(session, &ckptbase, &allocated, config));

    /* Return the array to our caller. */
    *ckptbasep = ckptbase;
    if (allocatedp != NULL)
        *allocatedp = allocated;

    if (0) {
err:
        __wt_meta_ckptlist_free(session, &ckptbase);
    }

    return (ret);
}

/*
 * __ckpt_load --
 *     Load a single checkpoint's information into a WT_CKPT structure.
 */
static int
__ckpt_load(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *k, WT_CONFIG_ITEM *v, WT_CKPT *ckpt)
{
    WT_CONFIG_ITEM a;
    WT_DECL_RET;
    char timebuf[64];

    /*
     * Copy the name, address (raw and hex), order and time into the slot. If there's no address,
     * it's a fake.
     */
    WT_RET(__wt_strndup(session, k->str, k->len, &ckpt->name));

    WT_RET(__wt_config_subgets(session, v, "addr", &a));
    WT_RET(__wt_buf_set(session, &ckpt->addr, a.str, a.len));
    if (a.len == 0)
        F_SET(ckpt, WT_CKPT_FAKE);
    else
        WT_RET(__wt_nhex_to_raw(session, a.str, a.len, &ckpt->raw));

    WT_RET(__wt_config_subgets(session, v, "order", &a));
    if (a.len == 0)
        goto format;
    ckpt->order = a.val;

    WT_RET(__wt_config_subgets(session, v, "time", &a));
    if (a.len == 0 || a.len > sizeof(timebuf) - 1)
        goto format;
    memcpy(timebuf, a.str, a.len);
    timebuf[a.len] = '\0';
    /* NOLINTNEXTLINE(cert-err34-c) */
    if (sscanf(timebuf, "%" SCNu64, &ckpt->sec) != 1)
        goto format;

    WT_RET(__wt_config_subgets(session, v, "size", &a));
    ckpt->size = (uint64_t)a.val;

    /* Default to durability. */
    WT_TIME_AGGREGATE_INIT(&ckpt->ta);

    ret = __wt_config_subgets(session, v, "oldest_start_ts", &a);
    WT_RET_NOTFOUND_OK(ret);
    if (ret != WT_NOTFOUND && a.len != 0)
        ckpt->ta.oldest_start_ts = (uint64_t)a.val;

    ret = __wt_config_subgets(session, v, "newest_txn", &a);
    WT_RET_NOTFOUND_OK(ret);
    if (ret != WT_NOTFOUND && a.len != 0)
        ckpt->ta.newest_txn = (uint64_t)a.val;

    ret = __wt_config_subgets(session, v, "newest_start_durable_ts", &a);
    WT_RET_NOTFOUND_OK(ret);
    if (ret != WT_NOTFOUND && a.len != 0)
        ckpt->ta.newest_start_durable_ts = (uint64_t)a.val;
    else {
        /*
         * Backward compatibility changes, as the parameter name is different in older versions of
         * WT, make sure that we read older format in case if we didn't find the newer format name.
         */
        ret = __wt_config_subgets(session, v, "start_durable_ts", &a);
        WT_RET_NOTFOUND_OK(ret);
        if (ret != WT_NOTFOUND && a.len != 0)
            ckpt->ta.newest_start_durable_ts = (uint64_t)a.val;
    }

    ret = __wt_config_subgets(session, v, "newest_stop_ts", &a);
    WT_RET_NOTFOUND_OK(ret);
    if (ret != WT_NOTFOUND && a.len != 0)
        ckpt->ta.newest_stop_ts = (uint64_t)a.val;

    ret = __wt_config_subgets(session, v, "newest_stop_txn", &a);
    WT_RET_NOTFOUND_OK(ret);
    if (ret != WT_NOTFOUND && a.len != 0)
        ckpt->ta.newest_stop_txn = (uint64_t)a.val;

    ret = __wt_config_subgets(session, v, "newest_stop_durable_ts", &a);
    WT_RET_NOTFOUND_OK(ret);
    if (ret != WT_NOTFOUND && a.len != 0)
        ckpt->ta.newest_stop_durable_ts = (uint64_t)a.val;
    else {
        /*
         * Backward compatibility changes, as the parameter name is different in older versions of
         * WT, make sure that we read older format in case if we didn't find the newer format name.
         */
        ret = __wt_config_subgets(session, v, "stop_durable_ts", &a);
        WT_RET_NOTFOUND_OK(ret);
        if (ret != WT_NOTFOUND && a.len != 0)
            ckpt->ta.newest_stop_durable_ts = (uint64_t)a.val;
    }

    ret = __wt_config_subgets(session, v, "prepare", &a);
    WT_RET_NOTFOUND_OK(ret);
    if (ret != WT_NOTFOUND && a.len != 0)
        ckpt->ta.prepare = (uint8_t)a.val;

    WT_RET(__wt_check_addr_validity(session, &ckpt->ta, false));

    WT_RET(__wt_config_subgets(session, v, "write_gen", &a));
    if (a.len == 0)
        goto format;
    ckpt->write_gen = (uint64_t)a.val;

    /*
     * If runtime write generation isn't supplied, this means that we're doing an upgrade and that
     * we're opening the tree for the first time. We should just leave it as 0 so it is recognized
     * as part of a previous run.
     */
    ret = __wt_config_subgets(session, v, "run_write_gen", &a);
    WT_RET_NOTFOUND_OK(ret);
    if (ret != WT_NOTFOUND && a.len != 0)
        ckpt->run_write_gen = (uint64_t)a.val;

    return (0);

format:
    WT_RET_MSG(session, WT_ERROR, "corrupted checkpoint list");
}

/*
 * __wt_metadata_update_base_write_gen --
 *     Update the connection's base write generation from the config string.
 */
int
__wt_metadata_update_base_write_gen(WT_SESSION_IMPL *session, const char *config)
{
    WT_CKPT ckpt;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);
    memset(&ckpt, 0, sizeof(ckpt));

    if ((ret = __ckpt_last(session, config, &ckpt)) == 0) {
        conn->base_write_gen = WT_MAX(ckpt.write_gen + 1, conn->base_write_gen);
        __wt_meta_checkpoint_free(session, &ckpt);
    } else
        WT_RET_NOTFOUND_OK(ret);

    return (0);
}

/*
 * __wt_metadata_init_base_write_gen --
 *     Initialize the connection's base write generation.
 */
int
__wt_metadata_init_base_write_gen(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    char *config;

    /* Initialize the base write gen to 1 */
    S2C(session)->base_write_gen = 1;
    /* Retrieve the metadata entry for the metadata file. */
    WT_ERR(__wt_metadata_search(session, WT_METAFILE_URI, &config));
    /* Update base write gen to the write gen of metadata. */
    WT_ERR(__wt_metadata_update_base_write_gen(session, config));

err:
    __wt_free(session, config);
    return (ret);
}

/*
 * __wt_metadata_correct_base_write_gen --
 *     Update the connection's base write generation from all files in metadata at then end of the
 *     recovery checkpoint.
 */
int
__wt_metadata_correct_base_write_gen(WT_SESSION_IMPL *session)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    char *config, *uri;

    uri = NULL;
    WT_RET(__wt_metadata_cursor(session, &cursor));
    while ((ret = cursor->next(cursor)) == 0) {
        WT_ERR(cursor->get_key(cursor, &uri));

        if (!WT_BTREE_PREFIX(uri))
            continue;

        WT_ERR(cursor->get_value(cursor, &config));

        /* Update base write gen to the write gen. */
        WT_ERR(__wt_metadata_update_base_write_gen(session, config));
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    if (ret != 0 && uri != NULL)
        __wt_err(session, ret, "unable to correct write gen for %s", uri);
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __wt_meta_ckptlist_to_meta --
 *     Convert a checkpoint list into its metadata representation.
 */
int
__wt_meta_ckptlist_to_meta(WT_SESSION_IMPL *session, WT_CKPT *ckptbase, WT_ITEM *buf)
{
    WT_CKPT *ckpt;
    const char *sep;

    sep = "";
    WT_RET(__wt_buf_fmt(session, buf, "checkpoint=("));
    WT_CKPT_FOREACH (ckptbase, ckpt) {
        /* Skip deleted checkpoints. */
        if (F_ISSET(ckpt, WT_CKPT_DELETE))
            continue;

        if (F_ISSET(ckpt, WT_CKPT_ADD | WT_CKPT_UPDATE)) {
            /*
             * We fake checkpoints for handles in the middle of a bulk load. If there is a
             * checkpoint, convert the raw cookie to a hex string.
             */
            if (ckpt->raw.size == 0)
                ckpt->addr.size = 0;
            else
                WT_RET(__wt_raw_to_hex(session, ckpt->raw.data, ckpt->raw.size, &ckpt->addr));
        }

        WT_RET(__wt_check_addr_validity(session, &ckpt->ta, false));

        WT_RET(__wt_buf_catfmt(session, buf, "%s%s", sep, ckpt->name));
        sep = ",";

        if (strcmp(ckpt->name, WT_CHECKPOINT) == 0)
            WT_RET(__wt_buf_catfmt(session, buf, ".%" PRId64, ckpt->order));

        /* Use PRId64 formats: WiredTiger's configuration code handles signed 8B values. */
        WT_RET(__wt_buf_catfmt(session, buf,
          "=(addr=\"%.*s\",order=%" PRId64 ",time=%" PRIu64 ",size=%" PRId64
          ",newest_start_durable_ts=%" PRId64 ",oldest_start_ts=%" PRId64 ",newest_txn=%" PRId64
          ",newest_stop_durable_ts=%" PRId64 ",newest_stop_ts=%" PRId64 ",newest_stop_txn=%" PRId64
          ",prepare=%d,write_gen=%" PRId64 ",run_write_gen=%" PRId64 ")",
          (int)ckpt->addr.size, (char *)ckpt->addr.data, ckpt->order, ckpt->sec,
          (int64_t)ckpt->size, (int64_t)ckpt->ta.newest_start_durable_ts,
          (int64_t)ckpt->ta.oldest_start_ts, (int64_t)ckpt->ta.newest_txn,
          (int64_t)ckpt->ta.newest_stop_durable_ts, (int64_t)ckpt->ta.newest_stop_ts,
          (int64_t)ckpt->ta.newest_stop_txn, (int)ckpt->ta.prepare, (int64_t)ckpt->write_gen,
          (int64_t)ckpt->run_write_gen));
    }
    WT_RET(__wt_buf_catfmt(session, buf, ")"));

    return (0);
}

/*
 * __wt_ckpt_blkmod_to_meta --
 *     Add in any modification block string needed, including an empty one.
 */
int
__wt_ckpt_blkmod_to_meta(WT_SESSION_IMPL *session, WT_ITEM *buf, WT_CKPT *ckpt)
{
    WT_BLOCK_MODS *blk;
    WT_ITEM bitstring;
    u_int i;
    bool skip_rename, valid;

    WT_CLEAR(bitstring);
    skip_rename = valid = false;
    for (i = 0, blk = &ckpt->backup_blocks[0]; i < WT_BLKINCR_MAX; ++i, ++blk)
        if (F_ISSET(blk, WT_BLOCK_MODS_VALID))
            valid = true;

    /*
     * If the existing block modifications are not valid, there is nothing to do.
     */
    if (!valid) {
        WT_RET(__wt_buf_catfmt(session, buf, ",checkpoint_backup_info="));
        return (0);
    }

    /*
     * We have at least one valid modified block list.
     */
    WT_RET(__wt_buf_catfmt(session, buf, ",checkpoint_backup_info=("));
    for (i = 0, blk = &ckpt->backup_blocks[0]; i < WT_BLKINCR_MAX; ++i, ++blk) {
        if (!F_ISSET(blk, WT_BLOCK_MODS_VALID))
            continue;

        /*
         * Occasionally skip including the rename string at all when it's not necessary for
         * correctness, that lets us simulate what is generated in the config string by earlier
         * versions of WiredTiger
         */
        if (FLD_ISSET(S2C(session)->timing_stress_flags, WT_TIMING_STRESS_BACKUP_RENAME) &&
          !F_ISSET(blk, WT_BLOCK_MODS_RENAME) && __wt_random(&session->rnd) % 10 == 0)
            skip_rename = true;

        WT_RET(__wt_raw_to_hex(session, blk->bitstring.data, blk->bitstring.size, &bitstring));
        WT_RET(__wt_buf_catfmt(session, buf,
          "%s\"%s\"=(id=%" PRIu32 ",granularity=%" PRIu64 ",nbits=%" PRIu64 ",offset=%" PRIu64
          "%s,blocks=%.*s)",
          i == 0 ? "" : ",", blk->id_str, i, blk->granularity, blk->nbits, blk->offset,
          skip_rename ? "" : F_ISSET(blk, WT_BLOCK_MODS_RENAME) ? ",rename=1" : ",rename=0",
          (int)bitstring.size, (char *)bitstring.data));
        /* The hex string length should match the appropriate number of bits. */
        WT_ASSERT(session, (blk->nbits >> 2) <= bitstring.size);
        __wt_buf_free(session, &bitstring);
    }
    WT_RET(__wt_buf_catfmt(session, buf, ")"));
    return (0);
}

/*
 * __wt_meta_ckptlist_update_config --
 *     Provided a metadata config and list of checkpoints, set a file's checkpoint value.
 */
int
__wt_meta_ckptlist_update_config(
  WT_SESSION_IMPL *session, WT_CKPT *ckptbase, const char *oldcfg, char **newcfgp)
{
    WT_CKPT *ckpt;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    char *newcfg;
    const char *cfg[3];

    newcfg = NULL;
    WT_RET(__wt_scr_alloc(session, 1024, &buf));
    WT_ERR(__wt_meta_ckptlist_to_meta(session, ckptbase, buf));

    /* Add backup block modifications for any added checkpoint. */
    WT_CKPT_FOREACH (ckptbase, ckpt)
        if (F_ISSET(ckpt, WT_CKPT_ADD))
            WT_ERR(__wt_ckpt_blkmod_to_meta(session, buf, ckpt));

    /* Replace the checkpoint entry. */
    cfg[0] = oldcfg;
    cfg[1] = buf->mem;
    cfg[2] = NULL;
    WT_ERR(__wt_config_collapse(session, cfg, &newcfg));

    *newcfgp = newcfg;

err:
    if (ret != 0)
        __wt_free(session, newcfg);
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_meta_ckptlist_set --
 *     Set a file's checkpoint value from the WT_CKPT list.
 */
int
__wt_meta_ckptlist_set(
  WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle, WT_CKPT *ckptbase, WT_LSN *ckptlsn)
{
    WT_CKPT *ckpt;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    const char *fname;
    bool has_lsn;

    fname = dhandle->name;
    WT_RET(__wt_scr_alloc(session, 1024, &buf));
    WT_ERR(__wt_meta_ckptlist_to_meta(session, ckptbase, buf));
    /* Add backup block modifications for any added checkpoint. */
    WT_CKPT_FOREACH (ckptbase, ckpt)
        if (F_ISSET(ckpt, WT_CKPT_ADD))
            WT_ERR(__wt_ckpt_blkmod_to_meta(session, buf, ckpt));

    has_lsn = ckptlsn != NULL;
    if (ckptlsn != NULL)
        WT_ERR(__wt_buf_catfmt(session, buf, ",checkpoint_lsn=(%" PRIu32 ",%" PRIuMAX ")",
          ckptlsn->l.file, (uintmax_t)ckptlsn->l.offset));

    if (dhandle->type == WT_DHANDLE_TYPE_TIERED)
        WT_ERR(__wt_tiered_set_metadata(session, (WT_TIERED *)dhandle, buf));
    WT_ERR(__ckpt_set(session, fname, buf->mem, has_lsn));

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_meta_ckptlist_free --
 *     Discard the checkpoint array.
 */
void
__wt_meta_ckptlist_free(WT_SESSION_IMPL *session, WT_CKPT **ckptbasep)
{
    WT_CKPT *ckpt, *ckptbase;

    if ((ckptbase = *ckptbasep) == NULL)
        return;

    /*
     * Sometimes the checkpoint list has a checkpoint which has not been named yet, but carries an
     * order number.
     */
    WT_CKPT_FOREACH_NAME_OR_ORDER (ckptbase, ckpt)
        __wt_meta_checkpoint_free(session, ckpt);
    __wt_free(session, *ckptbasep);
}

/*
 * __wt_meta_saved_ckptlist_free --
 *     Discard the saved checkpoint list.
 */
void
__wt_meta_saved_ckptlist_free(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;

    btree = S2BT(session);

    __wt_meta_ckptlist_free(session, &btree->ckpt);
    btree->ckpt_bytes_allocated = 0;
}

/*
 * __wt_meta_checkpoint_free --
 *     Clean up a single checkpoint structure.
 */
void
__wt_meta_checkpoint_free(WT_SESSION_IMPL *session, WT_CKPT *ckpt)
{
    WT_BLOCK_MODS *blk_mod;
    uint64_t i;

    if (ckpt == NULL)
        return;

    __wt_free(session, ckpt->name);
    __wt_free(session, ckpt->block_metadata);
    __wt_free(session, ckpt->block_checkpoint);
    __wt_buf_free(session, &ckpt->addr);
    __wt_buf_free(session, &ckpt->raw);
    __wt_free(session, ckpt->bpriv);
    for (i = 0; i < WT_BLKINCR_MAX; ++i) {
        blk_mod = &ckpt->backup_blocks[i];
        __wt_buf_free(session, &blk_mod->bitstring);
        __wt_free(session, blk_mod->id_str);
        F_CLR(blk_mod, WT_BLOCK_MODS_VALID);
    }

    WT_CLEAR(*ckpt); /* Clear to prepare for re-use. */
}

/*
 * __wt_meta_sysinfo_set --
 *     Set the system information in the metadata.
 */
int
__wt_meta_sysinfo_set(WT_SESSION_IMPL *session)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t oldest_timestamp;
    uint32_t snap_count;
    char hex_timestamp[WT_TS_HEX_STRING_SIZE];
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    txn_global = &S2C(session)->txn_global;

    txn = session->txn;
    WT_ERR(__wt_scr_alloc(session, 1024, &buf));
    /*
     * We need to record the timestamp of the checkpoint in the metadata. The timestamp value is set
     * at a higher level, either in checkpoint or in recovery.
     */
    __wt_timestamp_to_hex_string(txn_global->meta_ckpt_timestamp, hex_timestamp);

    /*
     * Don't leave a zero entry in the metadata: remove it. This avoids downgrade issues if the
     * metadata is opened with an older version of WiredTiger that does not understand the new
     * entry.
     */
    if (strcmp(hex_timestamp, "0") == 0)
        WT_ERR_NOTFOUND_OK(__wt_metadata_remove(session, WT_SYSTEM_CKPT_URI), false);
    else {
        WT_ERR(__wt_buf_fmt(session, buf, WT_SYSTEM_CKPT_TS "=\"%s\"", hex_timestamp));
        WT_ERR(__wt_metadata_update(session, WT_SYSTEM_CKPT_URI, buf->data));
    }

    /*
     * We also need to record the oldest timestamp in the metadata so we can set it on startup. We
     * should set the checkpoint's oldest timestamp as the minimum of the current oldest timestamp
     * and the checkpoint timestamp.
     *
     * Cache the oldest timestamp and use a read barrier to prevent us from reading two different
     * values of the oldest timestamp.
     */
    oldest_timestamp = txn_global->oldest_timestamp;
    WT_READ_BARRIER();
    __wt_timestamp_to_hex_string(
      WT_MIN(oldest_timestamp, txn_global->meta_ckpt_timestamp), hex_timestamp);
    if (strcmp(hex_timestamp, "0") == 0)
        WT_ERR_NOTFOUND_OK(__wt_metadata_remove(session, WT_SYSTEM_OLDEST_URI), false);
    else {
        WT_ERR(__wt_buf_fmt(session, buf, WT_SYSTEM_OLDEST_TS "=\"%s\"", hex_timestamp));
        WT_ERR(__wt_metadata_update(session, WT_SYSTEM_OLDEST_URI, buf->data));
    }

    /* Record snapshot information in metadata for checkpoint. */
    WT_ERR(__wt_buf_fmt(session, buf,
      WT_SYSTEM_CKPT_SNAPSHOT_MIN "=%" PRIu64 "," WT_SYSTEM_CKPT_SNAPSHOT_MAX "=%" PRIu64
                                  "," WT_SYSTEM_CKPT_SNAPSHOT_COUNT "=%" PRIu32,
      txn->snap_min, txn->snap_max, txn->snapshot_count));

    if (txn->snapshot_count > 0) {
        WT_ERR(__wt_buf_catfmt(session, buf, "," WT_SYSTEM_CKPT_SNAPSHOT "=["));
        for (snap_count = 0; snap_count < txn->snapshot_count - 1; ++snap_count)
            WT_ERR(__wt_buf_catfmt(session, buf, "%" PRIu64 "%s", txn->snapshot[snap_count], ","));

        WT_ERR(__wt_buf_catfmt(session, buf, "%" PRIu64 "%s", txn->snapshot[snap_count], "]"));
    }
    WT_ERR(__wt_metadata_update(session, WT_SYSTEM_CKPT_SNAPSHOT_URI, buf->data));

    __wt_verbose(session, WT_VERB_CHECKPOINT_PROGRESS,
      "saving checkpoint snapshot min: %" PRIu64 ", snapshot max: %" PRIu64
      " snapshot count: %" PRIu32
      ", oldest timestamp: %s , meta checkpoint timestamp: %s"
      " base write gen: %" PRIu64,
      txn->snap_min, txn->snap_max, txn->snapshot_count,
      __wt_timestamp_to_string(txn_global->oldest_timestamp, ts_string[0]),
      __wt_timestamp_to_string(txn_global->meta_ckpt_timestamp, ts_string[1]),
      S2C(session)->base_write_gen);

    /* Record the base write gen in metadata as part of checkpoint */
    WT_ERR(__wt_buf_fmt(
      session, buf, WT_SYSTEM_BASE_WRITE_GEN "=%" PRIu64, S2C(session)->base_write_gen));
    WT_ERR(__wt_metadata_update(session, WT_SYSTEM_BASE_WRITE_GEN_URI, buf->data));

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __ckpt_version_chk --
 *     Check the version major/minor numbers.
 */
static int
__ckpt_version_chk(WT_SESSION_IMPL *session, const char *fname, const char *config)
{
    WT_CONFIG_ITEM a, v;
    int majorv, minorv;

    WT_RET(__wt_config_getones(session, config, "version", &v));
    WT_RET(__wt_config_subgets(session, &v, "major", &a));
    majorv = (int)a.val;
    WT_RET(__wt_config_subgets(session, &v, "minor", &a));
    minorv = (int)a.val;

    if (majorv < WT_BTREE_MAJOR_VERSION_MIN || majorv > WT_BTREE_MAJOR_VERSION_MAX ||
      (majorv == WT_BTREE_MAJOR_VERSION_MIN && minorv < WT_BTREE_MINOR_VERSION_MIN) ||
      (majorv == WT_BTREE_MAJOR_VERSION_MAX && minorv > WT_BTREE_MINOR_VERSION_MAX))
        WT_RET_MSG(session, EACCES,
          "%s is an unsupported WiredTiger source file version %d.%d; this WiredTiger build only "
          "supports versions from %d.%d to %d.%d",
          fname, majorv, minorv, WT_BTREE_MAJOR_VERSION_MIN, WT_BTREE_MINOR_VERSION_MIN,
          WT_BTREE_MAJOR_VERSION_MAX, WT_BTREE_MINOR_VERSION_MAX);
    return (0);
}

/*
 * __wt_reset_blkmod --
 *     Reset the incremental backup information, and recreate incremental backup information to
 *     indicate copying the entire file.
 */
int
__wt_reset_blkmod(WT_SESSION_IMPL *session, const char *orig_config, WT_ITEM *buf)
{
    WT_CKPT ckpt;
    WT_DECL_RET;

    WT_CLEAR(ckpt);
    /*
     * Replace the old file entries with new file entries. We need to recreate the incremental
     * backup information to indicate copying the entire file in its bitmap.
     */
    /* First load any existing backup information into a temp checkpoint structure. */
    WT_RET(__meta_blk_mods_load(session, orig_config, NULL, &ckpt, true));

    /* Take the checkpoint structure and generate the metadata string. */
    ret = __wt_ckpt_blkmod_to_meta(session, buf, &ckpt);
    __wt_meta_checkpoint_free(session, &ckpt);
    return (ret);
}
