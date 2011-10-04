/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __verify_file --
 *	Verify a file.
 */
static int
__verify_file(WT_SESSION_IMPL *session, const char *cfg[])
{
	int ret;

	ret = __wt_verify(session, cfg);
	WT_TRET(__wt_session_release_btree(session));

	return (ret);
}

int
__wt_schema_verify(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
	WT_BTREE *cg;
	WT_BUF uribuf;
	WT_TABLE *table;
	const char *tablename;
	int i, ret;

	WT_CLEAR(uribuf);

	tablename = uri;
	ret = 0;

	/*
	 * Get the btree handle(s) to verify.
	 *
	 * Tell open that we're going to verify this handle, so it skips
	 * loading metadata such as the free list, which could be corrupted.
	 */
	if (WT_PREFIX_MATCH(uri, "file:")) {
		WT_RET(__wt_session_get_btree(session, uri, uri, NULL, cfg,
		    WT_BTREE_EXCLUSIVE | WT_BTREE_VERIFY));
		WT_RET(__verify_file(session, cfg));
	} else if (WT_PREFIX_MATCH(uri, "colgroup:") ||
	    WT_PREFIX_SKIP(uri, "index:")) {
		WT_RET(__wt_schema_get_btree(session, uri, strlen(uri), cfg,
		    WT_BTREE_EXCLUSIVE | WT_BTREE_VERIFY));
		WT_RET(__verify_file(session, cfg));
	} else if (WT_PREFIX_SKIP(tablename, "table:")) {
		WT_RET(__wt_schema_get_table(session,
		    tablename, strlen(tablename), &table));

		for (i = 0; i < WT_COLGROUPS(table); i++) {
			if ((cg = table->colgroup[i]) == NULL)
				continue;

			WT_TRET(__wt_schema_get_btree(session,
			    cg->name, strlen(cg->name), cfg,
			    WT_BTREE_EXCLUSIVE | WT_BTREE_VERIFY));
			WT_TRET(__verify_file(session, cfg));
		}
	} else {
		__wt_errx(session, "Unknown object type: %s", uri);
		return (EINVAL);
	}

	__wt_buf_free(session, &uribuf);
	return (ret);
}
