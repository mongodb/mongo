/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __lsm_meta_read_v0 --
 *     Read v0 of LSM metadata.
 */
static int
__lsm_meta_read_v0(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, const char *lsmconf)
{
    WT_CONFIG cparser, lparser;
    WT_CONFIG_ITEM ck, cv, fileconf, lk, lv, metadata;
    WT_DECL_RET;
    WT_LSM_CHUNK *chunk;
    u_int nchunks;

    chunk = NULL; /* -Wconditional-uninitialized */

    /* LSM trees inherit the merge setting from the connection. */
    if (F_ISSET(S2C(session), WT_CONN_LSM_MERGE))
        F_SET(lsm_tree, WT_LSM_TREE_MERGES);

    __wt_config_init(session, &cparser, lsmconf);
    while ((ret = __wt_config_next(&cparser, &ck, &cv)) == 0) {
        if (WT_STRING_MATCH("key_format", ck.str, ck.len)) {
            __wt_free(session, lsm_tree->key_format);
            WT_RET(__wt_strndup(session, cv.str, cv.len, &lsm_tree->key_format));
        } else if (WT_STRING_MATCH("value_format", ck.str, ck.len)) {
            __wt_free(session, lsm_tree->value_format);
            WT_RET(__wt_strndup(session, cv.str, cv.len, &lsm_tree->value_format));
        } else if (WT_STRING_MATCH("collator", ck.str, ck.len)) {
            if (cv.len == 0 || WT_STRING_MATCH("none", cv.str, cv.len))
                continue;
            /*
             * Extract the application-supplied metadata (if any) from the file configuration.
             */
            WT_RET(__wt_config_getones(session, lsmconf, "file_config", &fileconf));
            WT_CLEAR(metadata);
            WT_RET_NOTFOUND_OK(__wt_config_subgets(session, &fileconf, "app_metadata", &metadata));
            WT_RET(__wt_collator_config(session, lsm_tree->name, &cv, &metadata,
              &lsm_tree->collator, &lsm_tree->collator_owned));
            WT_RET(__wt_strndup(session, cv.str, cv.len, &lsm_tree->collator_name));
        } else if (WT_STRING_MATCH("bloom_config", ck.str, ck.len)) {
            __wt_free(session, lsm_tree->bloom_config);
            /* Don't include the brackets. */
            WT_RET(__wt_strndup(session, cv.str + 1, cv.len - 2, &lsm_tree->bloom_config));
        } else if (WT_STRING_MATCH("file_config", ck.str, ck.len)) {
            __wt_free(session, lsm_tree->file_config);
            /* Don't include the brackets. */
            WT_RET(__wt_strndup(session, cv.str + 1, cv.len - 2, &lsm_tree->file_config));
        } else if (WT_STRING_MATCH("auto_throttle", ck.str, ck.len)) {
            if (cv.val)
                F_SET(lsm_tree, WT_LSM_TREE_THROTTLE);
            else
                F_CLR(lsm_tree, WT_LSM_TREE_THROTTLE);
        } else if (WT_STRING_MATCH("bloom", ck.str, ck.len))
            lsm_tree->bloom = (uint32_t)cv.val;
        else if (WT_STRING_MATCH("bloom_bit_count", ck.str, ck.len))
            lsm_tree->bloom_bit_count = (uint32_t)cv.val;
        else if (WT_STRING_MATCH("bloom_hash_count", ck.str, ck.len))
            lsm_tree->bloom_hash_count = (uint32_t)cv.val;
        else if (WT_STRING_MATCH("chunk_count_limit", ck.str, ck.len)) {
            lsm_tree->chunk_count_limit = (uint32_t)cv.val;
            if (cv.val != 0)
                F_CLR(lsm_tree, WT_LSM_TREE_MERGES);
        } else if (WT_STRING_MATCH("chunk_max", ck.str, ck.len))
            lsm_tree->chunk_max = (uint64_t)cv.val;
        else if (WT_STRING_MATCH("chunk_size", ck.str, ck.len))
            lsm_tree->chunk_size = (uint64_t)cv.val;
        else if (WT_STRING_MATCH("merge_max", ck.str, ck.len))
            lsm_tree->merge_max = (uint32_t)cv.val;
        else if (WT_STRING_MATCH("merge_min", ck.str, ck.len))
            lsm_tree->merge_min = (uint32_t)cv.val;
        else if (WT_STRING_MATCH("last", ck.str, ck.len))
            lsm_tree->last = (u_int)cv.val;
        else if (WT_STRING_MATCH("chunks", ck.str, ck.len)) {
            __wt_config_subinit(session, &lparser, &cv);
            for (nchunks = 0; (ret = __wt_config_next(&lparser, &lk, &lv)) == 0;) {
                if (WT_STRING_MATCH("id", lk.str, lk.len)) {
                    WT_RET(__wt_realloc_def(
                      session, &lsm_tree->chunk_alloc, nchunks + 1, &lsm_tree->chunk));
                    WT_RET(__wt_calloc_one(session, &chunk));
                    lsm_tree->chunk[nchunks++] = chunk;
                    chunk->id = (uint32_t)lv.val;
                    WT_RET(__wt_lsm_tree_chunk_name(
                      session, lsm_tree, chunk->id, chunk->generation, &chunk->uri));
                    F_SET(chunk, WT_LSM_CHUNK_ONDISK | WT_LSM_CHUNK_STABLE);
                } else if (WT_STRING_MATCH("bloom", lk.str, lk.len)) {
                    WT_RET(
                      __wt_lsm_tree_bloom_name(session, lsm_tree, chunk->id, &chunk->bloom_uri));
                    F_SET(chunk, WT_LSM_CHUNK_BLOOM);
                    continue;
                } else if (WT_STRING_MATCH("chunk_size", lk.str, lk.len)) {
                    chunk->size = (uint64_t)lv.val;
                    continue;
                } else if (WT_STRING_MATCH("count", lk.str, lk.len)) {
                    chunk->count = (uint64_t)lv.val;
                    continue;
                } else if (WT_STRING_MATCH("generation", lk.str, lk.len)) {
                    chunk->generation = (uint32_t)lv.val;
                    continue;
                }
            }
            WT_RET_NOTFOUND_OK(ret);
            lsm_tree->nchunks = nchunks;
        } else if (WT_STRING_MATCH("old_chunks", ck.str, ck.len)) {
            __wt_config_subinit(session, &lparser, &cv);
            for (nchunks = 0; (ret = __wt_config_next(&lparser, &lk, &lv)) == 0;) {
                if (WT_STRING_MATCH("bloom", lk.str, lk.len)) {
                    WT_RET(__wt_strndup(session, lv.str, lv.len, &chunk->bloom_uri));
                    F_SET(chunk, WT_LSM_CHUNK_BLOOM);
                    continue;
                }
                WT_RET(__wt_realloc_def(
                  session, &lsm_tree->old_alloc, nchunks + 1, &lsm_tree->old_chunks));
                WT_RET(__wt_calloc_one(session, &chunk));
                lsm_tree->old_chunks[nchunks++] = chunk;
                WT_RET(__wt_strndup(session, lk.str, lk.len, &chunk->uri));
                F_SET(chunk, WT_LSM_CHUNK_ONDISK);
            }
            WT_RET_NOTFOUND_OK(ret);
            lsm_tree->nold_chunks = nchunks;
        }
        /*
         * Ignore any other values: the metadata entry might have been created by a future release,
         * with unknown options.
         */
    }
    WT_RET_NOTFOUND_OK(ret);
    return (0);
}

/*
 * __lsm_meta_read_v1 --
 *     Read v1 of LSM metadata.
 */
static int
__lsm_meta_read_v1(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, const char *lsmconf)
{
    WT_CONFIG lparser;
    WT_CONFIG_ITEM cv, lk, lv, metadata;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_LSM_CHUNK *chunk;
    u_int nchunks;
    char *fileconf;
    const char *file_cfg[] = {WT_CONFIG_BASE(session, file_config), NULL, NULL, NULL};

    chunk = NULL; /* -Wconditional-uninitialized */

    WT_ERR(__wt_config_getones(session, lsmconf, "key_format", &cv));
    WT_ERR(__wt_strndup(session, cv.str, cv.len, &lsm_tree->key_format));
    WT_ERR(__wt_config_getones(session, lsmconf, "value_format", &cv));
    WT_ERR(__wt_strndup(session, cv.str, cv.len, &lsm_tree->value_format));

    WT_ERR(__wt_config_getones(session, lsmconf, "collator", &cv));
    if (cv.len != 0 && !WT_STRING_MATCH("none", cv.str, cv.len)) {
        /* Extract the application-supplied metadata (if any). */
        WT_CLEAR(metadata);
        WT_ERR_NOTFOUND_OK(__wt_config_getones(session, lsmconf, "app_metadata", &metadata), false);
        WT_ERR(__wt_collator_config(
          session, lsm_tree->name, &cv, &metadata, &lsm_tree->collator, &lsm_tree->collator_owned));
        WT_ERR(__wt_strndup(session, cv.str, cv.len, &lsm_tree->collator_name));
    }

    /* lsm.merge_custom does not appear in all V1 LSM metadata. */
    lsm_tree->custom_generation = 0;
    if ((ret = __wt_config_getones(session, lsmconf, "lsm.merge_custom.start_generation", &cv)) ==
      0)
        lsm_tree->custom_generation = (uint32_t)cv.val;
    WT_ERR_NOTFOUND_OK(ret, false);
    if (lsm_tree->custom_generation != 0) {
        WT_ERR(__wt_config_getones(session, lsmconf, "lsm.merge_custom.prefix", &cv));
        WT_ERR(__wt_strndup(session, cv.str, cv.len, &lsm_tree->custom_prefix));

        WT_ERR(__wt_config_getones(session, lsmconf, "lsm.merge_custom.suffix", &cv));
        WT_ERR(__wt_strndup(session, cv.str, cv.len, &lsm_tree->custom_suffix));
    }

    WT_ERR(__wt_config_getones(session, lsmconf, "lsm.auto_throttle", &cv));
    if (cv.val)
        F_SET(lsm_tree, WT_LSM_TREE_THROTTLE);
    else
        F_CLR(lsm_tree, WT_LSM_TREE_THROTTLE);

    WT_ERR(__wt_config_getones(session, lsmconf, "lsm.bloom", &cv));
    FLD_SET(lsm_tree->bloom, (cv.val == 0 ? WT_LSM_BLOOM_OFF : WT_LSM_BLOOM_MERGED));
    WT_ERR(__wt_config_getones(session, lsmconf, "lsm.bloom_oldest", &cv));
    if (cv.val != 0)
        FLD_SET(lsm_tree->bloom, WT_LSM_BLOOM_OLDEST);

    if (FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OFF) &&
      FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OLDEST))
        WT_ERR_MSG(session, EINVAL,
          "Bloom filters can only be created on newest and oldest chunks if bloom filters are "
          "enabled");

    WT_ERR(__wt_config_getones(session, lsmconf, "lsm.bloom_bit_count", &cv));
    lsm_tree->bloom_bit_count = (uint32_t)cv.val;
    WT_ERR(__wt_config_getones(session, lsmconf, "lsm.bloom_config", &cv));
    /* Don't include the brackets. */
    if (cv.type == WT_CONFIG_ITEM_STRUCT) {
        cv.str++;
        cv.len -= 2;
    }
    WT_ERR(__wt_config_check(session, WT_CONFIG_REF(session, WT_SESSION_create), cv.str, cv.len));
    WT_ERR(__wt_strndup(session, cv.str, cv.len, &lsm_tree->bloom_config));
    WT_ERR(__wt_config_getones(session, lsmconf, "lsm.bloom_hash_count", &cv));
    lsm_tree->bloom_hash_count = (uint32_t)cv.val;

    WT_ERR(__wt_config_getones(session, lsmconf, "lsm.chunk_count_limit", &cv));
    lsm_tree->chunk_count_limit = (uint32_t)cv.val;
    if (cv.val == 0)
        F_SET(lsm_tree, WT_LSM_TREE_MERGES);
    else
        F_CLR(lsm_tree, WT_LSM_TREE_MERGES);
    WT_ERR(__wt_config_getones(session, lsmconf, "lsm.chunk_max", &cv));
    lsm_tree->chunk_max = (uint64_t)cv.val;
    WT_ERR(__wt_config_getones(session, lsmconf, "lsm.chunk_size", &cv));
    lsm_tree->chunk_size = (uint64_t)cv.val;

    if (lsm_tree->chunk_size > lsm_tree->chunk_max)
        WT_ERR_MSG(session, EINVAL,
          "Chunk size (chunk_size) must be smaller than or equal to the maximum chunk size "
          "(chunk_max)");

    WT_ERR(__wt_config_getones(session, lsmconf, "lsm.merge_max", &cv));
    lsm_tree->merge_max = (uint32_t)cv.val;
    WT_ERR(__wt_config_getones(session, lsmconf, "lsm.merge_min", &cv));
    lsm_tree->merge_min = (uint32_t)cv.val;

    if (lsm_tree->merge_min > lsm_tree->merge_max)
        WT_ERR_MSG(session, EINVAL, "LSM merge_min must be less than or equal to merge_max");

    WT_ERR(__wt_config_getones(session, lsmconf, "last", &cv));
    lsm_tree->last = (u_int)cv.val;
    WT_ERR(__wt_config_getones(session, lsmconf, "chunks", &cv));
    __wt_config_subinit(session, &lparser, &cv);
    for (nchunks = 0; (ret = __wt_config_next(&lparser, &lk, &lv)) == 0;) {
        if (WT_STRING_MATCH("id", lk.str, lk.len)) {
            WT_ERR(
              __wt_realloc_def(session, &lsm_tree->chunk_alloc, nchunks + 1, &lsm_tree->chunk));
            WT_ERR(__wt_calloc_one(session, &chunk));
            lsm_tree->chunk[nchunks++] = chunk;
            chunk->id = (uint32_t)lv.val;
            F_SET(chunk, WT_LSM_CHUNK_ONDISK | WT_LSM_CHUNK_STABLE);
        } else if (WT_STRING_MATCH("bloom", lk.str, lk.len)) {
            WT_ERR(__wt_lsm_tree_bloom_name(session, lsm_tree, chunk->id, &chunk->bloom_uri));
            F_SET(chunk, WT_LSM_CHUNK_BLOOM);
        } else if (WT_STRING_MATCH("chunk_size", lk.str, lk.len)) {
            chunk->size = (uint64_t)lv.val;
        } else if (WT_STRING_MATCH("count", lk.str, lk.len)) {
            chunk->count = (uint64_t)lv.val;
        } else if (WT_STRING_MATCH("generation", lk.str, lk.len)) {
            chunk->generation = (uint32_t)lv.val;
            /*
             * Id appears first, but we need both id and generation to create the name.
             */
            WT_ERR(__wt_lsm_tree_chunk_name(
              session, lsm_tree, chunk->id, chunk->generation, &chunk->uri));
        }
    }
    WT_ERR_NOTFOUND_OK(ret, false);
    lsm_tree->nchunks = nchunks;

    WT_ERR(__wt_config_getones(session, lsmconf, "old_chunks", &cv));
    __wt_config_subinit(session, &lparser, &cv);
    for (nchunks = 0; (ret = __wt_config_next(&lparser, &lk, &lv)) == 0;) {
        if (WT_STRING_MATCH("bloom", lk.str, lk.len)) {
            WT_ERR(__wt_strndup(session, lv.str, lv.len, &chunk->bloom_uri));
            F_SET(chunk, WT_LSM_CHUNK_BLOOM);
            continue;
        }
        WT_ERR(__wt_realloc_def(session, &lsm_tree->old_alloc, nchunks + 1, &lsm_tree->old_chunks));
        WT_ERR(__wt_calloc_one(session, &chunk));
        lsm_tree->old_chunks[nchunks++] = chunk;
        WT_ERR(__wt_strndup(session, lk.str, lk.len, &chunk->uri));
        F_SET(chunk, WT_LSM_CHUNK_ONDISK);
    }
    WT_ERR_NOTFOUND_OK(ret, false);
    lsm_tree->nold_chunks = nchunks;

    /*
     * Set up the config for each chunk.
     *
     * Make the memory_page_max double the chunk size, so application threads don't immediately try
     * to force evict the chunk when the worker thread clears the NO_EVICTION flag.
     */
    file_cfg[1] = lsmconf;
    WT_ERR(__wt_scr_alloc(session, 0, &buf));
    WT_ERR(__wt_buf_fmt(session, buf, "key_format=u,value_format=u,memory_page_max=%" PRIu64,
      2 * lsm_tree->chunk_size));
    file_cfg[2] = buf->data;
    WT_ERR(__wt_config_collapse(session, file_cfg, &fileconf));
    lsm_tree->file_config = fileconf;

/*
 * Ignore any other values: the metadata entry might have been created by a future release, with
 * unknown options.
 */
err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __lsm_meta_upgrade_v1 --
 *     Upgrade to v1 of LSM metadata.
 */
static int
__lsm_meta_upgrade_v1(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    const char *new_cfg[] = {WT_CONFIG_BASE(session, lsm_meta), NULL, NULL, NULL};

    /* Include the custom config that used to be embedded in file_config. */
    new_cfg[1] = lsm_tree->file_config;

    WT_ERR(__wt_scr_alloc(session, 0, &buf));
    WT_ERR(__wt_buf_fmt(
      session, buf, "key_format=%s,value_format=%s", lsm_tree->key_format, lsm_tree->value_format));

    WT_ERR(__wt_buf_catfmt(session, buf, ",collator=%s",
      lsm_tree->collator_name != NULL ? lsm_tree->collator_name : ""));

    WT_ERR(__wt_buf_catfmt(session, buf, ",lsm=("));

    WT_ERR(
      __wt_buf_catfmt(session, buf, "auto_throttle=%d", F_ISSET(lsm_tree, WT_LSM_TREE_THROTTLE)));

    WT_ERR(
      __wt_buf_catfmt(session, buf, ",bloom=%d", FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_MERGED)));
    WT_ERR(__wt_buf_catfmt(
      session, buf, ",bloom_oldest=%d", FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OLDEST)));
    WT_ERR(__wt_buf_catfmt(session, buf, ",bloom_bit_count=%" PRIu32, lsm_tree->bloom_bit_count));
    if (lsm_tree->bloom_config != NULL && strlen(lsm_tree->bloom_config) > 0)
        WT_ERR(__wt_buf_catfmt(session, buf, ",bloom_config=(%s)", lsm_tree->bloom_config));
    else
        WT_ERR(__wt_buf_catfmt(session, buf, ",bloom_config="));
    WT_ERR(__wt_buf_catfmt(session, buf, ",bloom_hash_count=%" PRIu32, lsm_tree->bloom_hash_count));

    WT_ERR(
      __wt_buf_catfmt(session, buf, ",chunk_count_limit=%" PRIu32, lsm_tree->chunk_count_limit));
    WT_ERR(__wt_buf_catfmt(session, buf, ",chunk_max=%" PRIu64, lsm_tree->chunk_max));
    WT_ERR(__wt_buf_catfmt(session, buf, ",merge_max=%" PRIu32, lsm_tree->merge_max));
    WT_ERR(__wt_buf_catfmt(session, buf, ",merge_min=%" PRIu32, lsm_tree->merge_min));

    WT_ERR(__wt_buf_catfmt(session, buf, ")"));

    new_cfg[2] = buf->data;
    WT_ERR(__wt_config_merge(session, new_cfg, NULL, &lsm_tree->config));

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_lsm_meta_read --
 *     Read the metadata for an LSM tree.
 */
int
__wt_lsm_meta_read(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    char *lsmconf;
    bool upgrade;

    /* LSM trees inherit the merge setting from the connection. */
    if (F_ISSET(S2C(session), WT_CONN_LSM_MERGE))
        F_SET(lsm_tree, WT_LSM_TREE_MERGES);

    WT_RET(__wt_metadata_search(session, lsm_tree->name, &lsmconf));

    upgrade = false;
    ret = __wt_config_getones(session, lsmconf, "file_config", &cval);
    if (ret == 0) {
        ret = __lsm_meta_read_v0(session, lsm_tree, lsmconf);
        __wt_free(session, lsmconf);
        WT_RET(ret);
        upgrade = true;
    } else if (ret == WT_NOTFOUND) {
        lsm_tree->config = lsmconf;
        ret = 0;
        WT_RET(__lsm_meta_read_v1(session, lsm_tree, lsmconf));
    }
    /*
     * If the default merge_min was not overridden, calculate it now.
     */
    if (lsm_tree->merge_min < 2)
        lsm_tree->merge_min = WT_MAX(2, lsm_tree->merge_max / 2);
    /*
     * If needed, upgrade the configuration. We need to do this after we have fixed the merge_min
     * value.
     */
    if (upgrade)
        WT_RET(__lsm_meta_upgrade_v1(session, lsm_tree));
    return (ret);
}

/*
 * __wt_lsm_meta_write --
 *     Write the metadata for an LSM tree.
 */
int
__wt_lsm_meta_write(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, const char *newconfig)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_LSM_CHUNK *chunk;
    u_int i;
    char *new_metadata;
    const char *new_cfg[] = {NULL, NULL, NULL, NULL, NULL};
    bool first;

    new_metadata = NULL;

    WT_RET(__wt_scr_alloc(session, 0, &buf));
    WT_ERR(__wt_buf_catfmt(session, buf, ",last=%" PRIu32, lsm_tree->last));
    WT_ERR(__wt_buf_catfmt(session, buf, ",chunks=["));
    for (i = 0; i < lsm_tree->nchunks; i++) {
        chunk = lsm_tree->chunk[i];
        if (i > 0)
            WT_ERR(__wt_buf_catfmt(session, buf, ","));
        WT_ERR(__wt_buf_catfmt(session, buf, "id=%" PRIu32, chunk->id));
        WT_ERR(__wt_buf_catfmt(session, buf, ",generation=%" PRIu32, chunk->generation));
        if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
            WT_ERR(__wt_buf_catfmt(session, buf, ",bloom"));
        if (chunk->size != 0)
            WT_ERR(__wt_buf_catfmt(session, buf, ",chunk_size=%" PRIu64, chunk->size));
        if (chunk->count != 0)
            WT_ERR(__wt_buf_catfmt(session, buf, ",count=%" PRIu64, chunk->count));
    }
    WT_ERR(__wt_buf_catfmt(session, buf, "]"));
    WT_ERR(__wt_buf_catfmt(session, buf, ",old_chunks=["));
    first = true;
    for (i = 0; i < lsm_tree->nold_chunks; i++) {
        chunk = lsm_tree->old_chunks[i];
        WT_ASSERT(session, chunk != NULL);
        if (first)
            first = false;
        else
            WT_ERR(__wt_buf_catfmt(session, buf, ","));
        WT_ERR(__wt_buf_catfmt(session, buf, "\"%s\"", chunk->uri));
        if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
            WT_ERR(__wt_buf_catfmt(session, buf, ",bloom=\"%s\"", chunk->bloom_uri));
    }
    WT_ERR(__wt_buf_catfmt(session, buf, "]"));

    /* Update the existing configuration with the new values. */
    new_cfg[0] = WT_CONFIG_BASE(session, lsm_meta);
    new_cfg[1] = lsm_tree->config;
    new_cfg[2] = buf->data;
    new_cfg[3] = newconfig;
    WT_ERR(__wt_config_collapse(session, new_cfg, &new_metadata));
    ret = __wt_metadata_update(session, lsm_tree->name, new_metadata);
    WT_ERR(ret);

err:
    __wt_scr_free(session, &buf);
    __wt_free(session, new_metadata);
    return (ret);
}
