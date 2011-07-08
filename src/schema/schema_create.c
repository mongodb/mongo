/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int
__create_file(
    WT_SESSION_IMPL *session, const char *name, int intable, const char *config)
{
	const char *cfg[] = API_CONF_DEFAULTS(btree, meta, config);
	const char *treeconf;
	int exists;

	if (__wt_session_get_btree(session, name, strlen(name), NULL) == 0)
		return (0);

	exists = __wt_exist(name);
	if (!exists)
		WT_RET(__wt_btree_create(session, name));

	if (intable)
		WT_RET(__wt_strdup(session, config, &treeconf));
	else if (exists)
		WT_RET(__wt_btconf_read(session, name, &treeconf));
	else {
		WT_RET(__wt_config_collapse(session, cfg, &treeconf));
		WT_RET(__wt_btconf_write(session, name, treeconf));
	}

	/* Allocate a WT_BTREE handle, and open the underlying file. */
	WT_RET(__wt_btree_open(session, name, treeconf, 0));
	WT_RET(__wt_session_add_btree(session, NULL));
	return (0);
}

static int
__create_colgroup(
    WT_SESSION_IMPL *session, const char *name, const char *config)
{
	WT_BUF fmt;
	WT_CONFIG_ITEM cval;
	WT_TABLE *table;
	const char *cfg[] = { __wt_confdfl_colgroup_meta, config,
	    NULL, NULL, NULL };
	const char **cfgp, *cgconf, *cgname, *tablename;
	char *filename, *namebuf;
	size_t namelen, tlen;
	int ret;

	cgconf = namebuf = NULL;
	tablename = name + strlen("colgroup:");
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

	/* We're going to add some config strings together before collapsing. */
	cfgp = cfg + 2;
	WT_CLEAR(fmt);

	if (__wt_config_getones(session, config, "filename", &cval) == 0) {
		WT_RET(__wt_strndup(session, cval.str, cval.len, &namebuf));
		filename = namebuf;
	} else if (cgname != NULL) {
		namelen = sizeof("filename=_.wt") + tlen + strlen(cgname);
		WT_RET(__wt_calloc_def(session, namelen, &namebuf));
		snprintf(namebuf, namelen, "filename=%.*s_%s.wt",
		    (int)tlen, tablename, cgname);
		filename = namebuf + strlen("filename=");
		*cfgp++ = namebuf;
	} else {
		namelen = sizeof("filename=_.wt") + tlen;
		WT_RET(__wt_calloc_def(session, namelen, &namebuf));
		snprintf(namebuf, namelen, "filename=%.*s.wt",
		    (int)tlen, tablename);
		filename = namebuf + strlen("filename=");
		*cfgp++ = namebuf;
	}

	if (cgname != NULL) {
		if (__wt_config_getones(session,
		    config, "columns", &cval) != 0) {
			__wt_errx(session,
			    "No 'columns' configuration for '%s'", name);
			WT_ERR(EINVAL);
		}
		WT_ERR(__wt_buf_sprintf(session, &fmt, "value_format="));
		WT_ERR(__wt_struct_reformat(session,
		    table, cval.str, cval.len, 1, &fmt));
		*cfgp++ = fmt.data;
	}

	WT_ERR(__wt_config_collapse(session, cfg, &cgconf));
	WT_ERR(__wt_schema_table_insert(session, name, cgconf));
	WT_ERR(__create_file(session, filename, 1, cgconf));

	WT_ERR(__wt_schema_open_colgroups(session, table));

err:	__wt_free(session, namebuf);
	__wt_free(session, cgconf);
	__wt_buf_free(session, &fmt);
	return (ret);
}

static int
__create_index(WT_SESSION_IMPL *session, const char *name, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_TABLE *table;
	const char *cfg[] = { __wt_confdfl_index_meta, config, NULL, NULL };
	const char *idxconf, *idxname, *tablename;
	char *filename, *namebuf;
	size_t namelen, tlen;
	int ret;

	idxconf = namebuf = NULL;
	tablename = name + strlen("index:");
	idxname = strchr(tablename, ':');
	if (idxname == NULL) {
		__wt_errx(session, "Invalid index name, "
		     "should be <table name>:<colgroup name>: %s", name);
		return (EINVAL);
	}

	tlen = (size_t)(idxname - tablename);
	if ((ret = __wt_schema_get_table(session,
	    tablename, tlen, &table)) != 0) {
		__wt_errx(session,
		    "Can't create an index for a non-existent table: %.*s",
		    (int)tlen, tablename);
		return (ret);
	}

	++idxname;

	if (__wt_config_getones(session, config, "filename", &cval) == 0) {
		WT_RET(__wt_strndup(session, cval.str, cval.len, &namebuf));
		filename = namebuf;
	} else {
		namelen = sizeof("filename=_.wti") + tlen + strlen(idxname);
		WT_RET(__wt_calloc_def(session, namelen, &namebuf));
		snprintf(namebuf, namelen, "filename=%.*s_%s.wti",
		    (int)tlen, tablename, idxname);
		filename = namebuf + strlen("filename=");
		cfg[2] = namebuf;
	}

	WT_ERR(__wt_config_collapse(session, cfg, &idxconf));
	WT_ERR(__wt_schema_table_insert(session, name, idxconf));
	WT_ERR(__create_file(session, filename, 1, idxconf));

err:	__wt_free(session, idxconf);
	__wt_free(session, namebuf);
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
	if (strncmp(name, "colgroup:", 6) == 0)
		return (__create_colgroup(session, name, config));
	else if (strncmp(name, "file:", 5) == 0)
		return (__create_file(session,
		    name + strlen("file:"), 0, config));
	else if (strncmp(name, "index:", 6) == 0)
		return (__create_index(session, name, config));
	else if (strncmp(name, "schema:", 7) == 0)
		return (__create_file(session,
		    name + strlen("schema:"), 1, config));
	else if (strncmp(name, "table:", 6) == 0)
		return (__create_table(session, name, config));
	else {
		__wt_errx(session, "Unknown object type: %s", name);
		return (EINVAL);
	}
}
