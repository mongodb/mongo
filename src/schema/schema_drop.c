/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __drop_file --
 *	WT_SESSION::drop for a file.
 */
static int
__drop_file(WT_SESSION_IMPL *session, const char *uri)
{
	WT_BTREE_SESSION *btree_session;
	const char *name;
	int exist, ret;

	name = uri;
	if (!WT_PREFIX_SKIP(name, "file:")) {
		__wt_errx(session, "Expected a 'file:' URI: %s", uri);
		return (EINVAL);
	}

	/* If open, close the btree handle. */
	switch ((ret = __wt_session_find_btree(session,
	    name, strlen(name), NULL, WT_BTREE_EXCLUSIVE,
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

	WT_ERR(__wt_schema_table_remove(session, uri));

	WT_ERR(__wt_exist(session, name, &exist));
	if (exist)
		ret = __wt_remove(session, name);

err:	return (ret);
}

/*
 * __drop_table --
 *	WT_SESSION::drop for a table.
 */
static int
__drop_table(WT_SESSION_IMPL *session, const char *uri)
{
	WT_BUF *tmp;
	WT_CURSOR *cursor;
	WT_TABLE *table;
	const char *key, *name, *value;
	int ret;

	tmp = NULL;
	cursor = NULL;
	ret = 0;

	name = uri;
	if (!WT_PREFIX_SKIP(name, "table:")) {
		__wt_errx(session, "Expected a 'table:' URI: %s", uri);
		return (EINVAL);
	}

	/* Close the table if it's open. */
	if (__wt_schema_find_table(session, name, strlen(name), &table) == 0)
		WT_RET(__wt_schema_remove_table(session, table));

	WT_RET(__wt_scr_alloc(session, 100, &tmp));

	/*
	 * Open a cursor on the schema file and walk it, removing all table
	 * references.  For each index or colgroup, find the underlying file
	 * and remove it as well.
	 */
	WT_ERR(__wt_schema_table_cursor(session, &cursor));
	while ((ret = cursor->next(cursor)) == 0) {
		if ((ret = cursor->get_key(cursor, &key)) != 0) {
			__wt_err(session, ret, "schema cursor.get_key");
			WT_ERR(ret);
		}

#define	__MATCH(key, s, name)						\
	(strncmp(key, s, strlen(s)) == 0 &&				\
	    strncmp(key + strlen(s), name, strlen(name)) == 0 ? 1 : 0)
		if (__MATCH(key, "table:", name)) {
			WT_ERR(cursor->remove(cursor));
			continue;
		}
		if (!__MATCH(key, "index:", name) &&
		    !__MATCH(key, "colgroup:", name))
			continue;

		if ((ret = cursor->get_value(cursor, &value)) != 0) {
			__wt_err(session, ret, "schema cursor.get_value");
			WT_ERR(ret);
		}
		if ((value = strstr(value, "filename=")) == NULL) {
			__wt_err(session,
			    ret, "corrupted schema file: %s missing filename "
			    "configuration string",
			    key);
			WT_ERR(EINVAL);
		}
		WT_ERR(__wt_buf_fmt(
		    session, tmp, "file:%s", value + strlen("filename=")));
		WT_ERR(__drop_file(session, tmp->data));

		WT_ERR(cursor->remove(cursor));
	}

err:	if (cursor != NULL)
		WT_TRET(cursor->close(cursor, NULL));

	__wt_scr_free(&tmp);
	return (ret);
}

int
__wt_schema_drop(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
	WT_UNUSED(cfg);

	if (WT_PREFIX_MATCH(uri, "file:"))
		return (__drop_file(session, uri));
	if (WT_PREFIX_MATCH(uri, "table:"))
		return (__drop_table(session, uri));

	__wt_errx(session, "Unknown object type: %s", uri);
	return (EINVAL);
}
