/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __meta_btree_apply --
 *	Apply a function to all files listed in the metadata, apart from the
 *	metadata file.
 */
static inline int
__meta_btree_apply(WT_SESSION_IMPL *session, WT_CURSOR *cursor,
    int (*file_func)(WT_SESSION_IMPL *, const char *[]),
    int (*name_func)(WT_SESSION_IMPL *, const char *, bool *),
    const char *cfg[])
{
	WT_DECL_RET;
	const char *uri;
	bool skip;

	while ((ret = cursor->next(cursor)) == 0) {
		WT_RET(cursor->get_key(cursor, &uri));
		if (strcmp(uri, WT_METAFILE_URI) == 0)
			continue;

		skip = false;
		if (name_func != NULL)
			WT_RET(name_func(session, uri, &skip));

		if (file_func == NULL || skip || !WT_PREFIX_MATCH(uri, "file:"))
			continue;

		/*
		 * We need to pull the handle into the session handle cache
		 * and make sure it's referenced to stop other internal code
		 * dropping the handle (e.g in LSM when cleaning up obsolete
		 * chunks).  Holding the metadata lock isn't enough.
		 */
		if ((ret = __wt_session_get_btree(
		    session, uri, NULL, NULL, 0)) != 0)
			return (ret == EBUSY ? 0 : ret);
		WT_SAVE_DHANDLE(session, ret = file_func(session, cfg));
		if (WT_META_TRACKING(session))
			WT_TRET(__wt_meta_track_handle_lock(
			    session, false));
		else
			WT_TRET(__wt_session_release_btree(session));
		WT_RET(ret);
	}
	WT_RET_NOTFOUND_OK(ret);

	return (0);
}

/*
 * __wt_meta_apply_all --
 *	Apply a function to all files listed in the metadata, apart from the
 *	metadata file.
 */
int
__wt_meta_apply_all(WT_SESSION_IMPL *session,
    int (*file_func)(WT_SESSION_IMPL *, const char *[]),
    int (*name_func)(WT_SESSION_IMPL *, const char *, bool *),
    const char *cfg[])
{
	WT_CURSOR *cursor;
	WT_DECL_RET;

	WT_RET(__wt_metadata_cursor(session, &cursor));
	WT_SAVE_DHANDLE(session, ret =
	    __meta_btree_apply(session, cursor, file_func, name_func, cfg));
	WT_TRET(__wt_metadata_cursor_release(session, &cursor));

	return (ret);
}
