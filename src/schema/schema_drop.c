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
	WT_BTREE_SESSION *btree_session;
	int ret;

	/* If open, close the btree handle. */
	if ((ret = __wt_session_get_btree(session,
	    filename, strlen(filename), &btree_session)) == 0) {
		/*
		 * XXX fail gracefully if other threads have the tree open.
		 * It only matters that they don't have cursors open, we need
		 * a count in WT_BTREE_SESSION, plus some synchronization
		 * when first "pinning" a btree handle.
		 */
		WT_ASSERT(session, btree_session->btree->refcnt == 1);

		F_SET(btree_session->btree, WT_BTREE_NO_EVICTION);
		WT_TRET(__wt_session_remove_btree(session, btree_session));
	} else if (ret != 0)
		return (ret);

	/* TODO: use the connection home directory. */
	return (__wt_remove(session, filename));
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

		for (i = 0; i < table->ncolgroups; i++) {
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
