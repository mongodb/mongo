/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_schema_add_table --
 *	Add a table handle to the session's cache.
 */
int
__wt_schema_add_table(
    WT_SESSION_IMPL *session, WT_TABLE *table)
{
	TAILQ_INSERT_HEAD(&session->tables, table, q);

	return (0);
}

/*
 * __wt_schema_get_table --
 *	Get the table handle for the named table.
 */
int
__wt_schema_find_table(WT_SESSION_IMPL *session,
    const char *name, size_t namelen, WT_TABLE **tablep)
{
	WT_TABLE *table;
	const char *tablename;

	TAILQ_FOREACH(table, &session->tables, q) {
		tablename = table->name;
		(void)WT_PREFIX_SKIP(tablename, "table:");
		if (strncmp(tablename, name, namelen) == 0 &&
		    tablename[namelen] == '\0') {
			*tablep = table;
			return (0);
		}
	}

	return (WT_NOTFOUND);
}

/*
 * __wt_schema_get_table --
 *	Get the table handle for the named table.
 */
int
__wt_schema_get_table(WT_SESSION_IMPL *session,
    const char *name, size_t namelen, WT_TABLE **tablep)
{
	WT_DECL_RET;

	ret = __wt_schema_find_table(session, name, namelen, tablep);

	if (ret == WT_NOTFOUND) {
		WT_RET(__wt_schema_open_table(session, name, namelen, tablep));
		ret = __wt_schema_add_table(session, *tablep);
	}

	return (ret);
}

/*
 * __wt_schema_destroy_table --
 *	Free a table handle.
 */
void
__wt_schema_destroy_table(WT_SESSION_IMPL *session, WT_TABLE *table)
{
	int i;

	__wt_free(session, table->name);
	__wt_free(session, table->config);
	__wt_free(session, table->plan);
	__wt_free(session, table->key_format);
	__wt_free(session, table->value_format);
	__wt_free(session, table->colgroup);
	__wt_free(session, table->index);
	if (table->cg_name != NULL) {
		for (i = 0; i < WT_COLGROUPS(table); i++)
			__wt_free(session, table->cg_name[i]);
		__wt_free(session, table->cg_name);
	}
	if (table->idx_name != NULL) {
		for (i = 0; i < table->nindices; i++)
			__wt_free(session, table->idx_name[i]);
		__wt_free(session, table->idx_name);
	}
	__wt_free(session, table);
}

/*
 * __wt_schema_remove_table --
 *	Remove the table handle from the session, closing if necessary.
 */
int
__wt_schema_remove_table(
    WT_SESSION_IMPL *session, WT_TABLE *table)
{
	TAILQ_REMOVE(&session->tables, table, q);
	__wt_schema_destroy_table(session, table);

	return (0);
}

/*
 * __wt_schema_close_tables --
 *	Close all of the tables in a session.
 */
int
__wt_schema_close_tables(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_TABLE *table;

	while ((table = TAILQ_FIRST(&session->tables)) != NULL)
		WT_TRET(__wt_schema_remove_table(session, table));

	return (ret);
}

/*
 * __wt_schema_detach tree --
 *	Remove any references to a tree from a table in the session.
 *
 * Note: this function should be called with an exclusive lock on the btree
 * handle to prevent races.
 */
void
__wt_schema_detach_tree(WT_SESSION_IMPL *session, WT_BTREE *btree)
{
	WT_TABLE *table;
	int i;

	TAILQ_FOREACH(table, &session->tables, q) {
		/* Check the column groups. */
		for (i = 0; i < WT_COLGROUPS(table); i++)
			if (table->colgroup[i] == btree) {
				table->colgroup[i] = NULL;
				table->cg_complete = 0;
				return;
			}

		/* Check the indices. */
		for (i = 0; i < table->nindices; i++)
			if (table->index[i] == btree) {
				table->index[i] = NULL;
				table->idx_complete = 0;
				return;
			}
	}
}
