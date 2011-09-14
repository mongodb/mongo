/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int
__drop_file(WT_SESSION_IMPL *session, const char *filename)
{
	WT_BUF keybuf;
	WT_BTREE_SESSION *btree_session;
	int ret;

	WT_CLEAR(keybuf);

	/* If open, close the btree handle. */
	switch ((ret = __wt_session_find_btree(session,
	    filename, strlen(filename), NULL, WT_BTREE_EXCLUSIVE,
	    &btree_session))) {
	case 0:
		/*
		 * XXX We have an exclusive lock, which means there are no
		 * cursors open but some other thread may have the handle
		 * cached.
		 */
		WT_ASSERT(session, btree_session->btree->refcnt == 1);
		WT_TRET(__wt_session_remove_btree(session, btree_session));
		break;
	case WT_NOTFOUND:
		ret = 0;
		break;
	default:
		return (ret);
	}

	WT_RET(__wt_buf_sprintf(session, &keybuf, "file:%s", filename));
	WT_ERR(__wt_schema_table_remove(session, keybuf.data));

	/* TODO: use the connection home directory. */
	if (__wt_exist(filename))
		ret = __wt_remove(session, filename);

err:	__wt_buf_free(session, &keybuf);
	return (ret);
}

int
__wt_schema_drop(WT_SESSION_IMPL *session, const char *name, const char *config)
{
	WT_BTREE *cg;
	WT_TABLE *table;
	char *namebuf;
	const char *fullname;
	int i, ret;

	WT_UNUSED(config);

	fullname = name;
	namebuf = NULL;
	ret = 0;

	if (WT_PREFIX_SKIP(name, "file:")) {
		WT_RET(__drop_file(session, name));
	} else if (WT_PREFIX_SKIP(name, "table:")) {
		WT_RET(__wt_schema_get_table(session,
		    name, strlen(name), &table));

		for (i = 0; i < WT_COLGROUPS(table); i++) {
			if ((cg = table->colgroup[i]) == NULL)
				continue;
			table->colgroup[i] = NULL;

			/* Remove the schema table entry. */
			WT_TRET(__wt_schema_table_remove(session, cg->name));

			/*
			 * Remove the file.  The btree handle will be closed
			 * during the call, so we make a copy of the filename.
			 */
			WT_ERR(__wt_realloc(session, NULL,
			    strlen(cg->filename) + 1, &namebuf));
			strcpy(namebuf, cg->filename);
			WT_TRET(__drop_file(session, namebuf));
		}

		/* TODO: drop the indices. */

		WT_TRET(__wt_schema_remove_table(session, table));
		WT_TRET(__wt_schema_table_remove(session, fullname));
	} else {
		__wt_errx(session, "Unknown object type: %s", fullname);
		return (EINVAL);
	}

err:	__wt_free(session, namebuf);
	return (ret);
}
