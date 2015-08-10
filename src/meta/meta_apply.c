/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_meta_btree_apply --
 *	Apply a function to all files listed in the metadata, apart from the
 *	metadata file.
 */
int
__wt_meta_btree_apply(WT_SESSION_IMPL *session,
    int (*func)(WT_SESSION_IMPL *, const char *[]), const char *cfg[])
{
	WT_CURSOR *cursor;
	WT_DATA_HANDLE *saved_dhandle;
	WT_DECL_RET;
	const char *uri;
	int cmp, tret;

	saved_dhandle = session->dhandle;
	WT_RET(__wt_metadata_cursor(session, NULL, &cursor));
	cursor->set_key(cursor, "file:");
	if ((tret = cursor->search_near(cursor, &cmp)) == 0 && cmp < 0)
		tret = cursor->next(cursor);
	for (; tret == 0; tret = cursor->next(cursor)) {
		WT_ERR(cursor->get_key(cursor, &uri));
		if (!WT_PREFIX_MATCH(uri, "file:"))
			break;
		if (strcmp(uri, WT_METAFILE_URI) == 0)
			continue;

		/*
		 * We need to pull the handle into the session handle cache
		 * and make sure it's referenced to stop other internal code
		 * dropping the handle (e.g in LSM when cleaning up obsolete
		 * chunks).  Holding the metadata lock isn't enough.
		 */
		ret = __wt_session_get_btree(session, uri, NULL, NULL, 0);
		if (ret == 0) {
			WT_SAVE_DHANDLE(session,
			    ret = func(session, cfg));
			if (WT_META_TRACKING(session))
				WT_TRET(
				    __wt_meta_track_handle_lock(session, 0));
			else
				WT_TRET(__wt_session_release_btree(session));
		} else if (ret == EBUSY)
			ret = __wt_conn_btree_apply_single(
			    session, uri, NULL, func, cfg);
		WT_ERR(ret);
	}

	if (tret != WT_NOTFOUND)
		WT_TRET(tret);
err:	WT_TRET(cursor->close(cursor));
	session->dhandle = saved_dhandle;
	return (ret);
}
