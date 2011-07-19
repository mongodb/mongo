/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

int
__wt_schema_colgroup_name(WT_SESSION_IMPL *session,
    WT_TABLE *table, const char *cgname, size_t len, char **namebufp)
{
	char *namebuf;
	size_t namesize;

	namebuf = *namebufp;

	/* The primary filename is in the table config. */
	if (table->is_simple) {
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

int
__wt_schema_open_colgroups(WT_SESSION_IMPL *session, WT_TABLE *table)
{
	WT_BTREE_SESSION *btree_session;
	WT_BUF plan;
	WT_CONFIG cparser;
	WT_CONFIG_ITEM ckey, cval;
	WT_CURSOR *cursor;
	char *cgname, *filename;
	const char *config, *config_copy;
	uint32_t plansize;
	int i, ret;

	if (table->is_complete)
		return (0);

	config_copy = NULL;
	cursor = NULL;
	cgname = filename = NULL;

	WT_RET(__wt_config_subinit(session, &cparser, &table->cgconf));

	/* Open the primary. */
	for (i = 0; i < table->ncolgroups; i++) {
		if (!table->is_simple)
			WT_ERR(__wt_config_next(&cparser, &ckey, &cval));
		else
			WT_CLEAR(ckey);
		if (table->colgroup[i] != NULL)
			continue;
		/* Get the filename from the schema table. */
		WT_ERR(__wt_schema_colgroup_name(session, table,
		    ckey.str, ckey.len, &cgname));

		WT_ERR(__wt_schema_table_cursor(session, &cursor));
		cursor->set_key(cursor, cgname);
		if ((ret = cursor->search(cursor)) != 0) {
			/*
			 * It's okay at this point if the table is not
			 * yet complete.  Cursor opens will fail if
			 * table->is_complete is not set.
			 */
			if (ret == WT_NOTFOUND)
				ret = 0;
			goto err;
		}
		cursor->get_value(cursor, &config);

		WT_ERR(__wt_config_getones(session, config, "filename", &cval));
		if ((ret = __wt_session_get_btree(session,
		    cval.str, cval.len, &btree_session)) == 0) {
			table->colgroup[i] = btree_session->btree;
			goto end;
		}

		WT_ERR(__wt_realloc(session, NULL, cval.len + 1, &filename));
		memcpy(filename, cval.str, cval.len);

		if (!__wt_exist(filename)) {
			if (i == 0)
				__wt_errx(session, "Primary column group "
				    "for table '%s': file '%s' is missing",
				    table->name, filename);
			else
				__wt_errx(session, "Column group '%s' "
				    "created but the file '%s' is missing",
				    cgname, filename);
			ret = ENOENT;
			goto err;
		}

		/*
		 * The config string we pass in is freed along with the btree
		 * handle.
		 */
		WT_ERR(__wt_strdup(session, config, &config_copy));
		WT_ERR(__wt_btree_open(session,
		    cgname, filename, config_copy, 0));
		config_copy = NULL;
		WT_ERR(__wt_session_add_btree(session, NULL));
		table->colgroup[i] = session->btree;

end:		if (cursor != NULL) {
			ret = cursor->close(cursor, NULL);
			cursor = NULL;
			WT_ERR(ret);
		}
	}

	if (!table->is_simple) {
		WT_ERR(__wt_table_check(session, table));

		WT_CLEAR(plan);
		WT_ERR(__wt_struct_plan(session,
		    table, table->colconf.str, table->colconf.len, &plan));
		__wt_buf_steal(session, &plan, &table->plan, &plansize);
	}

	table->is_complete = 1;

err:	__wt_free(session, config_copy);
	__wt_free(session, cgname);
	__wt_free(session, filename);
	if (cursor != NULL)
		WT_TRET(cursor->close(cursor, NULL));
	return (ret);
}

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

	if (table->ncolgroups == 0) {
		/* We need a pointer to the primary. */
		table->ncolgroups = 1;
		table->is_simple = 1;
	}

	WT_ERR(__wt_calloc_def(session, table->ncolgroups, &table->colgroup));
	WT_ERR(__wt_schema_open_colgroups(session, table));

	*tablep = table;

err:	if (cursor != NULL)
		WT_TRET(cursor->close(cursor, NULL));
	__wt_free(session, tablename);
	return (ret);
}
