/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int
__create_file(WT_SESSION_IMPL *session,
    const char *name, const char *fileuri, const char *config)
{
	const char *cfg[] = API_CONF_DEFAULTS(session, create, config);
	const char *dropcfg[] = API_CONF_DEFAULTS(session, drop, "force");
	const char *filecfg[] = API_CONF_DEFAULTS(file, meta, config);
	const char *filename, *treeconf;
	int is_schema, ret;

	filename = fileuri;
	if (!WT_PREFIX_SKIP(filename, "file:")) {
		__wt_errx(session, "Expecting a 'file:' URI: %s", fileuri);
		return (EINVAL);
	}

	/*
	 * Opening the schema table is a special case, use the config
	 * string we were passed to open the file.
	 */
	is_schema = (strcmp(filename, WT_SCHEMA_FILENAME) == 0);

	/* If the file exists, don't try to recreate it. */
	if ((ret = __wt_session_get_btree(session, name, fileuri,
	    is_schema ? config : NULL, NULL, WT_BTREE_NO_LOCK)) != WT_NOTFOUND)
		return (ret);

	WT_RET(__wt_btree_create(session, filename));

	if (is_schema)
		WT_ERR(__wt_strdup(session, config, &treeconf));
	else
		WT_ERR(__wt_config_collapse(session, filecfg, &treeconf));
	WT_ERR(__wt_schema_table_insert(session, fileuri, treeconf));

	/* Allocate a WT_BTREE handle, and open the underlying file. */
	WT_ERR(__wt_btree_open(session, name, filename, treeconf, cfg, 0));
	treeconf = NULL;
	WT_ERR(__wt_session_add_btree(session, NULL));

	if (0) {
		/* If something goes wrong, throw away anything we created. */
err:		(void)__wt_drop_file(session, fileuri, dropcfg);
	}

	__wt_free(session, treeconf);
	return (ret);
}

static int
__create_colgroup(
    WT_SESSION_IMPL *session, const char *name, const char *config)
{
	WT_BUF fmt, namebuf, uribuf;
	WT_CONFIG_ITEM cval;
	WT_TABLE *table;
	const char *cfg[] = { __wt_confdfl_colgroup_meta, config, NULL, NULL };
	const char *filecfg[] = { config, NULL, NULL };
	const char **cfgp;
	const char *cgconf, *cgname, *fileconf, *filename, *fileuri, *tablename;
	size_t tlen;
	int ret;

	cgconf = fileconf = NULL;
	WT_CLEAR(fmt);
	WT_CLEAR(namebuf);
	WT_CLEAR(uribuf);

	tablename = name;
	if (!WT_PREFIX_SKIP(tablename, "colgroup:"))
		return (EINVAL);
	cgname = strchr(tablename, ':');
	if (cgname != NULL) {
		tlen = (size_t)(cgname - tablename);
		++cgname;
	} else
		tlen = strlen(tablename);

	if ((ret = __wt_schema_get_table(session,
	    tablename, tlen, &table)) != 0) {
		__wt_errx(session,
		    "Can't create '%s' for non-existent table %.*s",
		    name, (int)tlen, tablename);
		return (ret);
	}

	/* Make sure the column group is referenced from the table. */
	if (cgname != NULL && (ret = __wt_config_subgets(session,
	    &table->cgconf, cgname, &cval)) != 0) {
		__wt_errx(session,
		    "Column group '%s' not found in table '%.*s'",
		    cgname, (int)tlen, tablename);
		return (EINVAL);
	}

	/* Find the first NULL entry in the cfg stack. */
	for (cfgp = &cfg[1]; *cfgp; cfgp++)
		;

	/* Add the filename to the colgroup config before collapsing. */
	if (__wt_config_getones(session, config, "filename", &cval) == 0) {
		WT_ERR(__wt_buf_fmt(
		    session, &namebuf, "%.*s", (int)cval.len, cval.str));
		filename = namebuf.data;
	} else {
		if (cgname == NULL)
			WT_ERR(__wt_buf_fmt(session, &namebuf,
			    "filename=%.*s.wt", (int)tlen, tablename));
		else
			WT_ERR(__wt_buf_fmt(session, &namebuf,
			    "filename=%.*s_%s.wt", (int)tlen, tablename,
			    cgname));
		*cfgp++ = filename = namebuf.data;
		(void)WT_PREFIX_SKIP(filename, "filename=");
	}

	WT_ERR(__wt_config_collapse(session, cfg, &cgconf));

	/* Calculate the key/value formats -- these go into the file config. */
	WT_ERR(__wt_buf_fmt(session, &fmt, "key_format=%s", table->key_format));
	if (cgname == NULL)
		WT_ERR(__wt_buf_catfmt
		    (session, &fmt, ",value_format=%s", table->value_format));
	else {
		if (__wt_config_getones(session,
		    config, "columns", &cval) != 0) {
			__wt_errx(session,
			    "No 'columns' configuration for '%s'", name);
			WT_ERR(EINVAL);
		}
		WT_ERR(__wt_buf_catfmt(session, &fmt, ",value_format="));
		WT_ERR(__wt_struct_reformat(session,
		    table, cval.str, cval.len, NULL, 1, &fmt));
	}
	filecfg[1] = fmt.data;
	WT_ERR(__wt_config_concat(session, filecfg, &fileconf));

	WT_ERR(__wt_buf_fmt(session, &uribuf, "file:%s", filename));
	fileuri = uribuf.data;

	WT_ERR(__create_file(session, name, fileuri, fileconf));
	WT_ERR(__wt_schema_table_insert(session, name, cgconf));

	WT_ERR(__wt_schema_open_colgroups(session, table));

err:    __wt_free(session, cgconf);
	__wt_free(session, fileconf);
	__wt_buf_free(session, &fmt);
	__wt_buf_free(session, &namebuf);
	__wt_buf_free(session, &uribuf);
	return (ret);
}

static int
__create_index(WT_SESSION_IMPL *session, const char *name, const char *config)
{
	WT_BUF extra_cols, fmt, namebuf, uribuf;
	WT_CONFIG pkcols;
	WT_CONFIG_ITEM ckey, cval, icols;
	WT_TABLE *table;
	const char *cfg[] = { __wt_confdfl_index_meta, config, NULL, NULL };
	const char *filecfg[] = { config, NULL, NULL };
	const char *fileconf, *filename, *fileuri, *idxconf, *idxname;
	const char *tablename;
	size_t tlen;
	int i, ret;

	idxconf = fileconf = NULL;
	WT_CLEAR(fmt);
	WT_CLEAR(extra_cols);
	WT_CLEAR(namebuf);
	WT_CLEAR(uribuf);

	tablename = name;
	if (!WT_PREFIX_SKIP(tablename, "index:"))
		return (EINVAL);
	idxname = strchr(tablename, ':');
	if (idxname == NULL) {
		__wt_errx(session, "Invalid index name, "
		     "should be <table name>:<index name>: %s", name);
		return (EINVAL);
	}

	tlen = (size_t)(idxname++ - tablename);
	if ((ret = __wt_schema_get_table(session,
	    tablename, tlen, &table)) != 0) {
		__wt_errx(session,
		    "Can't create an index for a non-existent table: %.*s",
		    (int)tlen, tablename);
		return (ret);
	}

	/* Add the filename to the index config before collapsing. */
	if (__wt_config_getones(session, config, "filename", &cval) == 0) {
		WT_ERR(__wt_buf_fmt(session,
		    &namebuf, "%.*s", (int)cval.len, cval.str));
		filename = namebuf.data;
	} else {
		WT_ERR(__wt_buf_fmt(session, &namebuf,
		    "filename=%.*s_%s.wti", (int)tlen, tablename, idxname));
		cfg[2] = filename = namebuf.data;
		(void)WT_PREFIX_SKIP(filename, "filename=");
	}
	WT_ERR(__wt_config_collapse(session, cfg, &idxconf));

	/* Calculate the key/value formats -- these go into the file config. */
	if (__wt_config_getones(session,
	    config, "columns", &icols) != 0) {
		__wt_errx(session,
		    "No 'columns' configuration for '%s'", name);
		WT_ERR(EINVAL);
	}

	/*
	 * The key format for an index is somewhat subtle: the application
	 * specifies a set of columns that it will use for the key, but the
	 * engine usually adds some hidden columns in order to derive the
	 * primary key.  These hidden columns are part of the file's
	 * key_format, which we are calculating now, but not part of an index
	 * cursor's key_format.
	 */
	WT_ERR(__wt_config_subinit(session, &pkcols, &table->colconf));
	for (i = 0; i < table->nkey_columns &&
	    (ret = __wt_config_next(&pkcols, &ckey, &cval)) == 0;
	    i++) {
		/*
		 * If the primary key column is already in the secondary key,
		 * don't add it again.
		 */
		if (__wt_config_subgetraw(session, &icols, &ckey, &cval) == 0)
			continue;
		WT_ERR(__wt_buf_catfmt(
		    session, &extra_cols, "%.*s,", (int)ckey.len, ckey.str));
	}
	if (ret != 0 && ret != WT_NOTFOUND)
		goto err;
	/* Index values are empty: all columns are packed into the index key. */
	WT_ERR(__wt_buf_fmt(session, &fmt, "value_format=,key_format="));
	WT_ERR(__wt_struct_reformat(session, table,
	     icols.str, icols.len, (const char *)extra_cols.data, 0, &fmt));
	filecfg[1] = fmt.data;
	WT_ERR(__wt_config_concat(session, filecfg, &fileconf));

	WT_ERR(__wt_buf_fmt(session, &uribuf, "file:%s", filename));
	fileuri = uribuf.data;

	WT_ERR(__create_file(session, name, fileuri, fileconf));
	WT_ERR(__wt_schema_table_insert(session, name, idxconf));

err:	__wt_free(session, fileconf);
	__wt_free(session, idxconf);
	__wt_buf_free(session, &fmt);
	__wt_buf_free(session, &extra_cols);
	__wt_buf_free(session, &namebuf);
	__wt_buf_free(session, &uribuf);
	return (ret);
}

static int
__create_table(WT_SESSION_IMPL *session, const char *name, const char *config)
{
	WT_CONFIG conf;
	WT_CONFIG_ITEM cgkey, cgval, cval;
	WT_TABLE *table;
	const char *cfg[] = { __wt_confdfl_table_meta, config, NULL, NULL };
	const char *tableconf, *tablename;
	char *cgname;
	size_t cgsize;
	int ncolgroups, ret;

	cgname = NULL;
	tableconf = NULL;
	tablename = name + strlen("table:");

	if ((ret = __wt_schema_get_table(session,
	    tablename, strlen(tablename), &table)) == 0) {
		if (__wt_config_getones(session,
		    config, "exclusive", &cval) == 0 && cval.val) {
			__wt_errx(session, "Table exists: %s", tablename);
			return (EEXIST);
		} else
			return (0);
	} else if (ret != WT_NOTFOUND)
		return (ret);

	WT_RET(__wt_config_gets(session, cfg, "colgroups", &cval));
	WT_RET(__wt_config_subinit(session, &conf, &cval));
	for (ncolgroups = 0;
	    (ret = __wt_config_next(&conf, &cgkey, &cgval)) == 0;
	    ncolgroups++)
		;
	if (ret != WT_NOTFOUND)
		return (ret);

	WT_RET(__wt_config_collapse(session, cfg, &tableconf));
	WT_ERR(__wt_schema_table_insert(session, name, tableconf));

	if (ncolgroups == 0) {
		cgsize = strlen("colgroup:") + strlen(tablename) + 1;
		WT_ERR(__wt_calloc_def(session, cgsize, &cgname));
		snprintf(cgname, cgsize, "colgroup:%s", tablename);
		WT_ERR(__create_colgroup(session, cgname, config));
	}

err:	__wt_free(session, cgname);
	__wt_free(session, tableconf);
	return (ret);
}

int
__wt_schema_create(
    WT_SESSION_IMPL *session, const char *name, const char *config)
{
	if (WT_PREFIX_MATCH(name, "colgroup:"))
		return (__create_colgroup(session, name, config));
	else if (WT_PREFIX_MATCH(name, "file:"))
		return (__create_file(session, name, name, config));
	else if (WT_PREFIX_MATCH(name, "index:"))
		return (__create_index(session, name, config));
	else if (WT_PREFIX_MATCH(name, "table:"))
		return (__create_table(session, name, config));
	else {
		__wt_errx(session, "Unknown object type: %s", name);
		return (EINVAL);
	}
}
