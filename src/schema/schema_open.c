/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_schema_colgroup_name --
 *	Get the URI for a column group.  This is used for schema table lookups.
 *	The only complexity here is that simple tables (with a single column
 *	group) use a simpler naming scheme.
 */
int
__wt_schema_colgroup_name(WT_SESSION_IMPL *session,
    WT_TABLE *table, const char *cgname, size_t len, char **namebufp)
{
	char *namebuf;
	size_t namesize;

	namebuf = *namebufp;

	/* The primary filename is in the table config. */
	if (table->ncolgroups == 0) {
		namesize = strlen("colgroup:") +
		    strlen(table->name) + 1;
		WT_RET(__wt_realloc(session, NULL, namesize, &namebuf));
		snprintf(namebuf, namesize, "colgroup:%s", table->name);
	} else {
		namesize = strlen("colgroup::") +
		    strlen(table->name) + len + 1;
		WT_RET(__wt_realloc(session, NULL, namesize, &namebuf));
		snprintf(namebuf, namesize, "colgroup:%s:%.*s",
		    table->name, (int)len, cgname);
	}

	*namebufp = namebuf;
	return (0);
}

/*
 * __wt_schema_get_btree --
 *	Get the btree (into session->btree) for the named schema object
 *	(either a column group or an index).
 */
int
__wt_schema_get_btree(WT_SESSION_IMPL *session, const char *objname, size_t len)
{
	WT_BUF uribuf;
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	const char *filename, *name, *objconf, *uri;
	int ret;

	cursor = NULL;
	WT_CLEAR(uribuf);

	name = objname;
	if (len != strlen(objname))
		WT_ERR(__wt_strndup(session, objname, len, &name));

	WT_ERR(__wt_schema_table_cursor(session, &cursor));
	cursor->set_key(cursor, name);
	WT_ERR(cursor->search(cursor));
	WT_ERR(cursor->get_value(cursor, &objconf));

	/* Get the filename from the schema table. */
	WT_ERR(__wt_config_getones(session, objconf, "filename", &cval));
	WT_ERR(__wt_buf_init(session, &uribuf, 0));
	WT_ERR(__wt_buf_sprintf(session, &uribuf, "file:%.*s",
	    (int)cval.len, cval.str));
	filename = uri = uribuf.data;
	(void)WT_PREFIX_SKIP(filename, "file:");

	/* !!! Close the schema cursor first, this overwrites session->btree. */
	ret = cursor->close(cursor, NULL);
	cursor = NULL;
	if (ret != 0)
		goto err;

	ret = __wt_session_get_btree(session, uri, filename, NULL);
	if (ret == ENOENT)
		__wt_errx(session,
		    "%s created but '%s' is missing", objname, uri);

err:	__wt_buf_free(session, &uribuf);
	if (name != objname)
		__wt_free(session, name);
	if (cursor != NULL)
		WT_TRET(cursor->close(cursor, NULL));
	return (ret);
}

/*
 * __wt_schema_open_colgroups --
 *	Open the column groups for a table.
 */
int
__wt_schema_open_colgroups(WT_SESSION_IMPL *session, WT_TABLE *table)
{
	WT_BUF plan;
	WT_CONFIG cparser;
	WT_CONFIG_ITEM ckey, cval;
	char *cgname;
	const char *fileconf;
	int i, ret;

	if (table->cg_complete)
		return (0);

	fileconf = NULL;
	cgname = NULL;
	ret = 0;

	WT_RET(__wt_config_subinit(session, &cparser, &table->cgconf));

	/* Open each column group. */
	for (i = 0; i < WT_COLGROUPS(table); i++) {
		if (table->ncolgroups > 0)
			WT_ERR(__wt_config_next(&cparser, &ckey, &cval));
		else
			WT_CLEAR(ckey);
		if (table->colgroup[i] != NULL)
			continue;

		WT_ERR(__wt_schema_colgroup_name(session, table,
		    ckey.str, ckey.len, &cgname));
		ret = __wt_schema_get_btree(session, cgname, strlen(cgname));
		if (ret != 0) {
			/* It is okay if the table is not yet complete. */
			if (ret == WT_NOTFOUND)
				ret = 0;
			goto err;
		}
		table->colgroup[i] = session->btree;
	}

	if (!table->is_simple) {
		WT_ERR(__wt_table_check(session, table));

		WT_CLEAR(plan);
		WT_ERR(__wt_struct_plan(session,
		    table, table->colconf.str, table->colconf.len, 1, &plan));
		table->plan = __wt_buf_steal(session, &plan, NULL);
	}

	table->cg_complete = 1;

err:	__wt_free(session, cgname);
	__wt_free(session, fileconf);
	return (ret);
}

/*
 * ___open_index --
 *	Open an index.
 */
static int
__open_index(WT_SESSION_IMPL *session, WT_TABLE *table,
    const char *uri, const char *idxconf, WT_BTREE **btreep)
{
	WT_BUF cols, plan, uribuf;
	WT_CONFIG colconf;
	WT_CONFIG_ITEM ckey, cval, icols;
	const char *filename, *fileuri;
	int i, ret;

	ret = 0;
	WT_CLEAR(uribuf);

	/* Get the filename from the index config. */
	WT_ERR(__wt_config_getones(session, idxconf, "filename", &cval));
	WT_ERR(__wt_buf_init(session, &uribuf, 0));
	WT_ERR(__wt_buf_sprintf(session, &uribuf, "file:%.*s",
	    (int)cval.len, cval.str));
	filename = fileuri = uribuf.data;
	(void)WT_PREFIX_SKIP(filename, "file:");

	ret = __wt_session_get_btree(session, fileuri, filename, NULL);
	if (ret == ENOENT)
		__wt_errx(session, "Index '%s' created but '%s' is missing",
		    uri, fileuri);
	/* Other errors will already have generated an error message. */
	if (ret != 0)
		goto err;

	/*
	 * The key format for an index is somewhat subtle: the application
	 * specifies a set of columns that it will use for the key, but the
	 * engine usually adds some hidden columns in order to derive the
	 * primary key.  These hidden columns are part of the file's key, which
	 * we are calculating now.
	 *
	 * Start with the declared index columns.
	 */
	WT_CLEAR(cols);
	WT_ERR(__wt_config_getones(session, idxconf, "columns", &icols));
	WT_ERR(__wt_config_subinit(session, &colconf, &icols));
	while ((ret = __wt_config_next(&colconf, &ckey, &cval)) == 0)
		WT_ERR(__wt_buf_sprintf(session, &cols, "%.*s,",
		    (int)ckey.len, ckey.str));
	if (ret != 0 && ret != WT_NOTFOUND)
		goto err;

	/*
	 * Now add any primary key columns from the table that are not
	 * already part of the index key.
	 */
	WT_ERR(__wt_config_subinit(session, &colconf, &table->colconf));
	for (i = 0; i < table->nkey_columns &&
	    (ret = __wt_config_next(&colconf, &ckey, &cval)) == 0;
	    i++) {
		/*
		 * If the primary key column is already in the secondary key,
		 * don't add it again.
		 */
		if (__wt_config_subgetraw(session, &icols, &ckey, &cval) == 0)
			continue;
		WT_ERR(__wt_buf_sprintf(session, &cols, "%.*s,",
		    (int)ckey.len, ckey.str));
	}
	if (ret != 0 && ret != WT_NOTFOUND)
		goto err;

	WT_CLEAR(plan);
	WT_ERR(__wt_struct_plan(session,
	    table, cols.data, cols.size, 0, &plan));
	session->btree->key_plan = __wt_buf_steal(session, &plan, NULL);

	/* By default, index cursor values are the table value columns. */
	/* XXX Optimize to use index columns in preference to table lookups. */
	WT_ERR(__wt_struct_plan(session,
	    table, table->colconf.str, table->colconf.len, 1, &plan));
	session->btree->value_plan = __wt_buf_steal(session, &plan, NULL);

	*btreep = session->btree;

err:	__wt_buf_free(session, &cols);
	__wt_buf_free(session, &uribuf);

	return (ret);
}

/*
 * __wt_schema_open_indices --
 *	Open the indices for a table.
 */
int
__wt_schema_open_index(
    WT_SESSION_IMPL *session, WT_TABLE *table, const char *idxname, size_t len)
{
	WT_CURSOR *cursor;
	int i, match, ret, skipped;
	const char *idxconf, *uri;

	cursor = NULL;
	skipped = 0;
	idxconf = NULL;

	if (len == 0 && table->idx_complete)
		return (0);

	/*
	 * XXX Do a full scan through the schema table to find all matching
	 * indices.  This scan be optimized when we have cursor search + next.
	 */
	WT_RET(__wt_schema_table_cursor(session, &cursor));

	/* Open each column group. */
	for (i = 0; (ret = cursor->next(cursor)) == 0;) {
		WT_ERR(cursor->get_key(cursor, &uri));
		if (!WT_PREFIX_SKIP(uri, "index:") ||
		    !WT_PREFIX_SKIP(uri, table->name) ||
		    !WT_PREFIX_SKIP(uri, ":"))
			continue;

		/* Is this the index we are looking for? */
		match = (len > 0 &&
		   strncmp(uri, idxname, len) == 0 && strlen(uri) == len);

		if (i * sizeof(WT_BTREE *) >= table->index_alloc)
			WT_ERR(__wt_realloc(session, &table->index_alloc,
			    WT_MAX(10 * sizeof(WT_BTREE *),
			    2 * table->index_alloc),
			    &table->index));

		if (table->index[i] == NULL) {
			if (len == 0 || match) {
				WT_ERR(cursor->get_value(cursor, &idxconf));
				WT_ERR(__open_index(session,
				    table, uri, idxconf, &table->index[i]));
			} else
				skipped = 1;
		}

		if (match) {
			ret = cursor->close(cursor, NULL);
			cursor = NULL;
			session->btree = table->index[i];
			break;
		}
		i++;
	}

	/* Did we make it all the way through? */
	if (ret == WT_NOTFOUND) {
		ret = 0;
		if (!skipped) {
			table->nindices = i;
			table->idx_complete = 1;
		}
	}

err:	if (cursor != NULL)
		WT_TRET(cursor->close(cursor, NULL));
	return (ret);
}

/*
 * __wt_schema_open_table --
 *	Open a named table.
 */
int
__wt_schema_open_table(WT_SESSION_IMPL *session,
    const char *name, size_t namelen, WT_TABLE **tablep)
{
	WT_CONFIG cparser;
	WT_CONFIG_ITEM ckey, cval;
	WT_CURSOR *cursor;
	WT_TABLE *table;
	const char *tconfig;
	char *tablename;
	size_t bufsize;
	int ret;

	bufsize = namelen + strlen("table:") + 1;
	WT_RET(__wt_calloc_def(session, bufsize, &tablename));
	snprintf(tablename, bufsize, "table:%.*s", (int)namelen, name);

	cursor = NULL;
	WT_ERR(__wt_schema_table_cursor(session, &cursor));
	cursor->set_key(cursor, tablename);
	WT_ERR(cursor->search(cursor));
	WT_ERR(cursor->get_value(cursor, &tconfig));

	WT_ERR(__wt_calloc_def(session, 1, &table));
	WT_ERR(__wt_strndup(session, name, namelen, &table->name));

	WT_ERR(__wt_config_getones(session, tconfig, "columns", &cval));
	table->is_simple = (cval.len == 0);

	WT_ERR(__wt_config_getones(session, tconfig, "key_format", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len, &table->key_format));
	WT_ERR(__wt_config_getones(session, tconfig, "value_format", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len, &table->value_format));
	WT_ERR(__wt_strdup(session, tconfig, &table->config));

	/* Point to some items in the copy to save re-parsing. */
	WT_ERR(__wt_config_getones(session, table->config,
	    "columns", &table->colconf));

	WT_ERR(__wt_config_getones(session, table->config,
	    "colgroups", &table->cgconf));

	/* Count the number of column groups; */
	WT_ERR(__wt_config_subinit(session, &cparser, &table->cgconf));
	table->ncolgroups = 0;
	while ((ret = __wt_config_next(&cparser, &ckey, &cval)) == 0)
		++table->ncolgroups;
	if (ret != WT_NOTFOUND)
		goto err;

	WT_ERR(__wt_calloc_def(session, WT_COLGROUPS(table), &table->colgroup));
	WT_ERR(__wt_schema_open_colgroups(session, table));

	*tablep = table;

err:	if (cursor != NULL)
		WT_TRET(cursor->close(cursor, NULL));
	__wt_free(session, tablename);
	return (ret);
}
