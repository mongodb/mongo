/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int
__drop_file(WT_SESSION_IMPL *session, const char *fileuri)
{
	WT_BTREE_SESSION *btree_session;
	const char *filename;
	int ret;

	filename = fileuri;

	if (!WT_PREFIX_SKIP(filename, "file:")) {
		__wt_errx(session, "Expected a 'file:' URI: %s", fileuri);
		return (EINVAL);
	}

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

	WT_ERR(__wt_schema_table_remove(session, fileuri));

	if (__wt_exist(session, filename))
		ret = __wt_remove(session, filename);

err:	return (ret);
}

int
__wt_schema_drop(WT_SESSION_IMPL *session, const char *uri, const char *config)
{
	WT_BTREE *cg;
	WT_BUF uribuf;
	WT_TABLE *table;
	char *namebuf;
	const char *fileuri, *tablename;
	int i, ret;

	WT_UNUSED(config);
	WT_CLEAR(uribuf);

	tablename = uri;
	namebuf = NULL;
	ret = 0;

	if (WT_PREFIX_MATCH(uri, "file:")) {
		WT_RET(__drop_file(session, uri));
	} else if (WT_PREFIX_SKIP(tablename, "table:")) {
		WT_RET(__wt_schema_get_table(session,
		    tablename, strlen(tablename), &table));

		for (i = 0; i < WT_COLGROUPS(table); i++) {
			if ((cg = table->colgroup[i]) == NULL)
				continue;
			table->colgroup[i] = NULL;

			WT_RET(__wt_buf_init(session, &uribuf, 0));
			WT_RET(__wt_buf_sprintf(session, &uribuf,
			    "file:%s", cg->filename));
			fileuri = uribuf.data;

			/* Remove the schema table entry. */
			WT_TRET(__wt_schema_table_remove(session, cg->name));

			/* Remove the file. */
			WT_TRET(__drop_file(session, fileuri));
		}

		/* TODO: drop the indices. */

		WT_TRET(__wt_schema_remove_table(session, table));
		WT_TRET(__wt_schema_table_remove(session, uri));
	} else {
		__wt_errx(session, "Unknown object type: %s", uri);
		return (EINVAL);
	}

	__wt_free(session, namebuf);
	__wt_buf_free(session, &uribuf);
	return (ret);
}
