/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curmetadata_search --
 *	WT_CURSOR->search method for the metadata cursor type.
 */
static int
__curmetadata_search(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	char *value;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, search, cbt->btree);

	WT_CURSOR_NEEDKEY(cursor);

	if (cursor->key.size == strlen("metadata") &&
	    strncmp(cursor->key.data, "metadata", strlen("metadata")) == 0) {
		WT_ERR(__wt_metadata_search(session,
		    "file:metadata", (const char **)&value));
		/*
		 * Copy the value in the underlying btree cursors tmp item
		 * which will be free'd when the cursor is closed.
		 */
		cbt->tmp.data = cbt->tmp.mem = value;
		cbt->tmp.size = cbt->tmp.memsize = strlen(value);
		/* TODO: Is this assignment OK? */
		cursor->value = cbt->tmp;
		F_SET(cursor, WT_CURSTD_VALUE_SET);
	} else
		WT_BTREE_CURSOR_SAVE_AND_RESTORE(
		    cursor, __wt_btcur_search(cbt), ret);

err:	API_END(session);
	return (ret);
}

/*
 * __wt_curmetadata_open --
 *	WT_SESSION->open_cursor method for metadata cursors.
 */
int
__wt_curmetadata_open(WT_SESSION_IMPL *session,
    const char *uri, WT_CURSOR *owner, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_UNUSED(uri);

	/* TODO validate the configuration. */
	WT_RET(__wt_curfile_open(
	    session, WT_METADATA_URI, owner, cfg, cursorp));

	/* Metadata search is special. */
	(*cursorp)->search = __curmetadata_search;

	/* User accessible metadata cursors can't modify the metadata. */
	(*cursorp)->insert = __wt_cursor_notsup;
	(*cursorp)->update = __wt_cursor_notsup;
	(*cursorp)->remove = __wt_cursor_notsup;

	return (0);
}
