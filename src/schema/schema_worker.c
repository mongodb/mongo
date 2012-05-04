/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_schema_worker --
 *	Get Btree handles for the object and cycle through calls to an
 * underlying worker function with each handle.
 */
int
__wt_schema_worker(WT_SESSION_IMPL *session,
   const char *uri, const char *cfg[],
   int (*func)(WT_SESSION_IMPL *, const char *[]), uint32_t open_flags)
{
	WT_DECL_RET;
	WT_TABLE *table;
	const char *cgname, *tablename;
	int i;

	tablename = uri;

	/* Get the btree handle(s) and call the underlying function. */
	if (WT_PREFIX_MATCH(uri, "file:")) {
		WT_RET(__wt_session_get_btree(session, uri, cfg, open_flags));
		ret = func(session, cfg);
		WT_TRET(__wt_session_release_btree(session));
	} else if (WT_PREFIX_MATCH(uri, "colgroup:") ||
	    WT_PREFIX_MATCH(uri, "index:")) {
		WT_RET(__wt_schema_get_btree(
		    session, uri, strlen(uri), cfg, open_flags));
		ret = func(session, cfg);
		WT_TRET(__wt_session_release_btree(session));
	} else if (WT_PREFIX_SKIP(tablename, "table:")) {
		WT_RET(__wt_schema_get_table(session,
		    tablename, strlen(tablename), &table));

		for (i = 0; i < WT_COLGROUPS(table); i++) {
			cgname = table->cg_name[i];
			WT_TRET(__wt_schema_get_btree(session,
			    cgname, strlen(cgname), cfg, open_flags));
			ret = func(session, cfg);
			WT_TRET(__wt_session_release_btree(session));
			WT_RET(ret);
		}
	} else
		return (__wt_unknown_object_type(session, uri));

	return (ret);
}
