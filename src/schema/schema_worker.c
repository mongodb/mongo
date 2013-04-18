/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
   const char *uri,
   int (*func)(WT_SESSION_IMPL *, const char *[]),
   const char *cfg[], uint32_t open_flags)
{
	WT_COLGROUP *colgroup;
	WT_DATA_SOURCE *dsrc;
	WT_DECL_RET;
	WT_INDEX *idx;
	WT_SESSION *wt_session;
	WT_TABLE *table;
	const char *tablename;
	u_int i;

	table = NULL;
	tablename = uri;

	/* Get the btree handle(s) and call the underlying function. */
	if (WT_PREFIX_MATCH(uri, "file:")) {
		WT_ERR(__wt_session_get_btree_ckpt(
		    session, uri, cfg, open_flags));
		ret = func(session, cfg);
		WT_TRET(__wt_session_release_btree(session));
	} else if (WT_PREFIX_MATCH(uri, "colgroup:")) {
		WT_ERR(__wt_schema_get_colgroup(session, uri, NULL, &colgroup));
		WT_ERR(__wt_schema_worker(
		    session, colgroup->source, func, cfg, open_flags));
	} else if (WT_PREFIX_SKIP(tablename, "index:")) {
		idx = NULL;
		WT_ERR(__wt_schema_get_index(session, uri, NULL, &idx));
		WT_ERR(__wt_schema_worker(
		    session, idx->source, func, cfg, open_flags));
	} else if (WT_PREFIX_MATCH(uri, "lsm:")) {
		WT_ERR(__wt_lsm_tree_worker(
		    session, uri, func, cfg, open_flags));
	} else if (WT_PREFIX_SKIP(tablename, "table:")) {
		WT_ERR(__wt_schema_get_table(session,
		    tablename, strlen(tablename), 0, &table));
		WT_ASSERT(session, session->dhandle == NULL);

		for (i = 0; i < WT_COLGROUPS(table); i++) {
			colgroup = table->cgroups[i];
			WT_ERR(__wt_schema_worker(
			    session, colgroup->source, func, cfg, open_flags));
		}

		WT_ERR(__wt_schema_open_indices(session, table));
		for (i = 0; i < table->nindices; i++) {
			idx = table->indices[i];
			WT_ERR(__wt_schema_worker(
			    session, idx->source, func, cfg, open_flags));
		}
	} else if ((ret = __wt_schema_get_source(session, uri, &dsrc)) == 0) {
		wt_session = (WT_SESSION *)session;
		if (func == __wt_compact && dsrc->compact != NULL)
			WT_ERR(dsrc->compact(
			    dsrc, wt_session, uri, (WT_CONFIG_ARG *)cfg));
		else if (func == __wt_salvage && dsrc->salvage != NULL)
			WT_ERR(dsrc->salvage(
			   dsrc, wt_session, uri, (WT_CONFIG_ARG *)cfg));
		else if (func == __wt_verify && dsrc->verify != NULL)
			WT_ERR(dsrc->verify(
			   dsrc, wt_session, uri, (WT_CONFIG_ARG *)cfg));
		else
			WT_ERR(__wt_object_unsupported(session, uri));
	} else
		WT_ERR(__wt_bad_object_type(session, uri));

err:	if (table != NULL)
		__wt_schema_release_table(session, table);
	return (ret);
}
