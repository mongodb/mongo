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
   int (*file_func)(WT_SESSION_IMPL *, const char *[]),
   int (*name_func)(WT_SESSION_IMPL *, const char *),
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

	if (name_func != NULL)
		WT_ERR(name_func(session, uri));

	/* Get the btree handle(s) and call the underlying function. */
	if (WT_PREFIX_MATCH(uri, "file:")) {
		if (file_func != NULL) {
			WT_ERR(__wt_session_get_btree_ckpt(
			    session, uri, cfg, open_flags));
			ret = file_func(session, cfg);
			WT_TRET(__wt_session_release_btree(session));
		}
	} else if (WT_PREFIX_MATCH(uri, "colgroup:")) {
		WT_ERR(__wt_schema_get_colgroup(session, uri, NULL, &colgroup));
		WT_ERR(__wt_schema_worker(session, colgroup->source,
		    file_func, name_func, cfg, open_flags));
	} else if (WT_PREFIX_SKIP(tablename, "index:")) {
		idx = NULL;
		WT_ERR(__wt_schema_get_index(session, uri, NULL, &idx));
		WT_ERR(__wt_schema_worker(session, idx->source,
		    file_func, name_func, cfg, open_flags));
	} else if (WT_PREFIX_MATCH(uri, "lsm:")) {
		WT_ERR(__wt_lsm_tree_worker(
		    session, uri, file_func, name_func, cfg, open_flags));
	} else if (WT_PREFIX_SKIP(tablename, "table:")) {
		WT_ERR(__wt_schema_get_table(session,
		    tablename, strlen(tablename), 0, &table));
		WT_ASSERT(session, session->dhandle == NULL);

		/*
		 * We could make a recursive call for each colgroup or index
		 * URI, but since we have already opened the table, we can take
		 * a short cut and skip straight to the sources.  If we have a
		 * name function, it needs to know about the intermediate URIs.
		 */
		for (i = 0; i < WT_COLGROUPS(table); i++) {
			colgroup = table->cgroups[i];
			if (name_func != NULL)
				WT_ERR(name_func(session, colgroup->name));
			WT_ERR(__wt_schema_worker(session, colgroup->source,
			    file_func, name_func, cfg, open_flags));
		}

		WT_ERR(__wt_schema_open_indices(session, table));
		for (i = 0; i < table->nindices; i++) {
			idx = table->indices[i];
			if (name_func != NULL)
				WT_ERR(name_func(session, idx->name));
			WT_ERR(__wt_schema_worker(session, idx->source,
			    file_func, name_func, cfg, open_flags));
		}
	} else if ((ret = __wt_schema_get_source(session, uri, &dsrc)) == 0) {
		wt_session = (WT_SESSION *)session;
		if (file_func == __wt_compact && dsrc->compact != NULL)
			WT_ERR(dsrc->compact(
			    dsrc, wt_session, uri, (WT_CONFIG_ARG *)cfg));
		else if (file_func == __wt_salvage && dsrc->salvage != NULL)
			WT_ERR(dsrc->salvage(
			   dsrc, wt_session, uri, (WT_CONFIG_ARG *)cfg));
		else if (file_func == __wt_verify && dsrc->verify != NULL)
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
