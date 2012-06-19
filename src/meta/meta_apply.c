/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
    int (*func)(WT_SESSION_IMPL *, const char *[]),
    const char *cfg[], uint32_t flags)
{
	WT_BTREE *saved_btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	const char *uri;
	int cmp, tret;

	saved_btree = session->btree;
	WT_RET(__wt_metadata_cursor(session, NULL, &cursor));
	cursor->set_key(cursor, "file:");
	if ((tret = cursor->search_near(cursor, &cmp)) == 0 && cmp < 0)
		tret = cursor->next(cursor);
	for (; tret == 0; tret = cursor->next(cursor)) {
		WT_ERR(cursor->get_key(cursor, &uri));
		if (!WT_PREFIX_MATCH(uri, "file:"))
			break;
		else if (strcmp(uri, WT_METADATA_URI) == 0)
			continue;
		WT_ERR(__wt_session_get_btree(session, uri, NULL, flags));
		ret = func(session, cfg);
		WT_TRET(__wt_session_release_btree(session));
		WT_ERR(ret);
	}

	if (tret != WT_NOTFOUND)
		WT_TRET(tret);
err:	WT_TRET(cursor->close(cursor));
	session->btree = saved_btree;
	return (ret);
}
