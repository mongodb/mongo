/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __sync_file --
 *	sync a file.
 */
static int
__sync_file(WT_SESSION_IMPL *session, const char *cfg[])
{
	int ret;

	ret = __wt_btree_sync(session, cfg);
	WT_TRET(__wt_session_release_btree(session));

	return (ret);
}

int
__wt_schema_sync(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
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
	 * Get the btree handle(s) to sync.
	 *
	 * Tell open that we're going to sync this handle, so it skips
	 * loading metadata such as the free list, which could be corrupted.
	 */
	if (WT_PREFIX_MATCH(uri, "file:")) {
		WT_RET(__wt_session_get_btree(session, uri, uri, NULL, cfg, 0));
		WT_RET(__sync_file(session, cfg));
	} else if (WT_PREFIX_MATCH(uri, "colgroup:") ||
	    WT_PREFIX_SKIP(uri, "index:")) {
		WT_RET(__wt_schema_get_btree(session,
		    uri, strlen(uri), cfg, 0));
		WT_RET(__sync_file(session, cfg));
	} else if (WT_PREFIX_SKIP(tablename, "table:")) {
		WT_RET(__wt_schema_get_table(session,
		    tablename, strlen(tablename), &table));

		for (i = 0; i < WT_COLGROUPS(table); i++) {
			if ((cg = table->colgroup[i]) == NULL)
				continue;

			WT_TRET(__wt_schema_get_btree(session,
			    cg->name, strlen(cg->name), cfg, 0));
			WT_TRET(__sync_file(session, cfg));
		}
	} else {
		__wt_errx(session, "Unknown object type: %s", uri);
		return (EINVAL);
	}

	__wt_buf_free(session, &uribuf);
	return (ret);
}
