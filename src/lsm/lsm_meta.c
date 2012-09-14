/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_lsm_meta_read --
 *	Read the metadata for an LSM tree.
 */
int
__wt_lsm_meta_read(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_CONFIG cparser, lparser;
	WT_CONFIG_ITEM ck, cv, lk, lv;
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	const char *config;
	int nchunks;
	size_t chunk_sz, alloc;

	WT_RET(__wt_metadata_read(session, lsm_tree->name, &config));
	WT_ERR(__wt_config_init(session, &cparser, config));
	while ((ret = __wt_config_next(&cparser, &ck, &cv)) == 0) {
		if (WT_STRING_MATCH("file_config", ck.str, ck.len)) {
			__wt_free(session, lsm_tree->file_config);
			/* Don't include the brackets. */
			WT_ERR(__wt_strndup(session,
			    cv.str + 1, cv.len - 2, &lsm_tree->file_config));
		} else if (WT_STRING_MATCH("key_format", ck.str, ck.len)) {
			__wt_free(session, lsm_tree->key_format);
			WT_ERR(__wt_strndup(session,
			    cv.str, cv.len, &lsm_tree->key_format));
		} else if (WT_STRING_MATCH("value_format", ck.str, ck.len)) {
			__wt_free(session, lsm_tree->value_format);
			WT_ERR(__wt_strndup(session,
			    cv.str, cv.len, &lsm_tree->value_format));
		} else if (WT_STRING_MATCH(
		    "lsm_bloom_bit_count", ck.str, ck.len))
			lsm_tree->bloom_bit_count = (uint32_t)cv.val;
		else if (WT_STRING_MATCH("lsm_bloom_hash_count", ck.str, ck.len))
			lsm_tree->bloom_hash_count = (uint32_t)cv.val;
		else if (WT_STRING_MATCH("threshold", ck.str, ck.len))
			lsm_tree->threshold = (uint32_t)cv.val;
		else if (WT_STRING_MATCH("last", ck.str, ck.len))
			lsm_tree->last = (uint32_t)cv.val;
		else if (WT_STRING_MATCH("chunks", ck.str, ck.len)) {
			WT_ERR(__wt_config_subinit(session, &lparser, &cv));
			chunk_sz = sizeof(*lsm_tree->chunk);
			for (nchunks = 0; (ret =
			    __wt_config_next(&lparser, &lk, &lv)) == 0; ) {
				if (WT_STRING_MATCH("bloom", lk.str, lk.len)) {
					WT_ERR(__wt_strndup(session,
					    lv.str, lv.len, &chunk->bloom_uri));
					continue;
				}
				if (WT_STRING_MATCH("count", lk.str, lk.len)) {
					chunk->count = lv.val;
					continue;
				}
				if ((nchunks + 1) * chunk_sz >
				    lsm_tree->chunk_alloc)
					WT_ERR(__wt_realloc(session,
					    &lsm_tree->chunk_alloc,
					    WT_MAX(10 * chunk_sz,
					    2 * lsm_tree->chunk_alloc),
					    &lsm_tree->chunk));
				WT_ERR(__wt_calloc_def(session, 1, &chunk));
				lsm_tree->chunk[nchunks++] = chunk;
				WT_ERR(__wt_strndup(session,
				    lk.str, lk.len, &chunk->uri));
				chunk->flags = WT_LSM_CHUNK_ONDISK;
			}
			WT_ERR_NOTFOUND_OK(ret);
			lsm_tree->nchunks = nchunks;
		} else if (WT_STRING_MATCH("old_chunks", ck.str, ck.len)) {
			WT_ERR(__wt_config_subinit(session, &lparser, &cv));
			chunk_sz = sizeof(*lsm_tree->old_chunks);
			for (nchunks = 0; (ret =
			    __wt_config_next(&lparser, &lk, &lv)) == 0; ) {
				if ((nchunks + 1) * chunk_sz >
				    lsm_tree->old_avail * chunk_sz) {
					alloc = lsm_tree->old_alloc;
					WT_ERR(__wt_realloc(session,
					    &lsm_tree->old_alloc,
					    chunk_sz * WT_MAX(10,
					    lsm_tree->nold_chunks +
					    2 * nchunks),
					    &lsm_tree->old_chunks));
					lsm_tree->nold_chunks = (int)
					    (lsm_tree->old_alloc / chunk_sz);
					lsm_tree->old_avail += (int)
					    ((lsm_tree->old_alloc - alloc) /
					    chunk_sz);
				}
				WT_ERR(__wt_calloc_def(session, 1, &chunk));
				lsm_tree->old_chunks[nchunks++] = chunk;
				WT_ERR(__wt_strndup(session,
				    lk.str, lk.len, &chunk->uri));
				chunk->flags = WT_LSM_CHUNK_ONDISK;
				--lsm_tree->old_avail;
			}
			WT_ERR_NOTFOUND_OK(ret);
			lsm_tree->nold_chunks = nchunks;
		} else
			WT_ERR(__wt_illegal_value(session, "LSM metadata"));

		/* TODO: collator */
	}
	WT_ERR_NOTFOUND_OK(ret);

err:	__wt_free(session, config);
	return (ret);
}

/*
 * __wt_lsm_meta_write --
 *	Write the metadata for an LSM tree.
 */
int
__wt_lsm_meta_write(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	int first, i;

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf,
	    "file_config=(%s),key_format=%s,value_format=%s",
	    lsm_tree->file_config,
	    lsm_tree->key_format, lsm_tree->value_format));
	WT_ERR(__wt_buf_catfmt(session, buf,
	    ",last=%" PRIu32 ",threshold=%" PRIu64
	    ",lsm_bloom_bit_count=%" PRIu32 ",lsm_bloom_hash_count=%" PRIu32,
	    lsm_tree->last, (uint64_t)lsm_tree->threshold,
	    lsm_tree->bloom_bit_count, lsm_tree->bloom_hash_count));
	WT_ERR(__wt_buf_catfmt(session, buf, ",chunks=["));
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = lsm_tree->chunk[i];
		if (i > 0)
			WT_ERR(__wt_buf_catfmt(session, buf, ","));
		WT_ERR(__wt_buf_catfmt(session, buf, "\"%s\"", chunk->uri));
		if (chunk->bloom_uri != NULL)
			WT_ERR(__wt_buf_catfmt(
			    session, buf, ",bloom=\"%s\"", chunk->bloom_uri));
		if (chunk->count != 0)
			WT_ERR(__wt_buf_catfmt(
			    session, buf, ",count=%" PRIu64, chunk->count));
	}
	WT_ERR(__wt_buf_catfmt(session, buf, "]"));
	WT_ERR(__wt_buf_catfmt(session, buf, ",old_chunks=["));
	first = 1;
	for (i = 0; i < (int)lsm_tree->nold_chunks; i++) {
		chunk = lsm_tree->old_chunks[i];
		if (chunk == NULL)
			continue;
		if (first)
			first = 0;
		else
			WT_ERR(__wt_buf_catfmt(session, buf, ","));
		WT_ERR(__wt_buf_catfmt(session, buf, "\"%s\"", chunk->uri));
	}
	WT_ERR(__wt_buf_catfmt(session, buf, "]"));
	WT_ERR(__wt_metadata_update(session, lsm_tree->name, buf->data));

err:	__wt_scr_free(&buf);
	return (ret);
}
