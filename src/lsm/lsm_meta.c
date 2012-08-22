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

	WT_RET(__wt_metadata_read(session, lsm_tree->name, &config));
	WT_ERR(__wt_config_init(session, &cparser, config));
	while ((ret = __wt_config_next(&cparser, &ck, &cv)) == 0) {
		if (WT_STRING_MATCH("file_config", ck.str, ck.len)) {
			__wt_free(session, lsm_tree->file_config);
			/* Don't include the brackets. */
			WT_ERR(__wt_strndup(session,
			    cv.str + 1, cv.len - 2, &lsm_tree->file_config));
		} else if (WT_STRING_MATCH("filename", ck.str, ck.len)) {
			__wt_free(session, lsm_tree->filename);
			WT_ERR(__wt_strndup(session,
			    cv.str, cv.len, &lsm_tree->filename));
		} else if (WT_STRING_MATCH("key_format", ck.str, ck.len)) {
			__wt_free(session, lsm_tree->key_format);
			WT_ERR(__wt_strndup(session,
			    cv.str, cv.len, &lsm_tree->key_format));
		} else if (WT_STRING_MATCH("value_format", ck.str, ck.len)) {
			__wt_free(session, lsm_tree->value_format);
			WT_ERR(__wt_strndup(session,
			    cv.str, cv.len, &lsm_tree->value_format));
		} else if (WT_STRING_MATCH("threshold", ck.str, ck.len))
			lsm_tree->threshold = (uint32_t)cv.val;
		else if (WT_STRING_MATCH("last", ck.str, ck.len))
			lsm_tree->last = (uint32_t)cv.val;
		else if (WT_STRING_MATCH("chunks", ck.str, ck.len)) {
			WT_ERR(__wt_config_subinit(session, &lparser, &cv));
			for (nchunks = 0;
			    (ret = __wt_config_next( &lparser, &lk, &lv)) == 0;
			    nchunks++) {
				if ((nchunks + 1) * sizeof(*lsm_tree->chunk) >
				    lsm_tree->chunk_alloc)
					WT_ERR(__wt_realloc(session,
					    &lsm_tree->chunk_alloc, WT_MAX(
					    10 * sizeof(*lsm_tree->chunk),
					    2 * lsm_tree->chunk_alloc),
					    &lsm_tree->chunk));
				chunk = &lsm_tree->chunk[nchunks];
				WT_ERR(__wt_strndup(session,
				    lk.str, lk.len, &chunk->uri));
				chunk->flags = WT_LSM_CHUNK_ONDISK;
			}
			WT_ERR_NOTFOUND_OK(ret);
			lsm_tree->nchunks = nchunks;
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
	int i;

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf,
	    "file_config=(%s),filename=\"%s\",key_format=%s,value_format=%s",
	    lsm_tree->file_config, lsm_tree->filename,
	    lsm_tree->key_format, lsm_tree->value_format));
	WT_ERR(__wt_buf_catfmt(session, buf,
	    ",last=%" PRIu32 ",threshold=%" PRIu64,
	    lsm_tree->last, (uint64_t)lsm_tree->threshold));
	WT_ERR(__wt_buf_catfmt(session, buf, ",chunks=["));
	for (i = 0; i < lsm_tree->nchunks; i++)
		WT_ERR(__wt_buf_catfmt(session, buf, "\"%s\"%s",
		    lsm_tree->chunk[i].uri,
		    (i < lsm_tree->nchunks - 1) ? "," : ""));
	WT_ERR(__wt_buf_catfmt(session, buf, "]"));
	WT_ERR(__wt_metadata_update(session, lsm_tree->name, buf->data));
#ifdef HAVE_DIAGNOSTIC
	{
	WT_LSM_TREE *test;
	WT_ERR(__wt_calloc_def(session, 1, &test));
	test->name = lsm_tree->name;
	WT_ERR(__wt_lsm_meta_read(session, test));
	__wt_free(session, test);
	}
#endif
err:	__wt_scr_free(&buf);
	return (ret);
}
