/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int
__create_btree(
    WT_SESSION_IMPL *session, const char *name, int intable, const char *config)
{
	const char *cfg[] = API_CONF_DEFAULTS(btree, meta, config);
	const char *treeconf;
	int exists;

	exists = __wt_exist(name);
	if (intable || !__wt_exist(name))
		WT_RET(__wt_config_collapse(session, cfg, &treeconf));
	else
		WT_RET(__wt_btconf_read(session, name, &treeconf));

	if (!exists) {
		WT_RET(__wt_btree_create(session, name));
		if (!intable)
			WT_RET(__wt_btconf_write(session, name, treeconf));
	} else if (__wt_session_get_btree(session,
	    name, strlen(name), NULL) == 0)
		return (0);

	/* Allocate a WT_BTREE handle, and open the underlying file. */
	WT_RET(__wt_btree_open(session, name, treeconf, 0));
	WT_RET(__wt_session_add_btree(session, NULL));
	return (0);
}

static int
__create_colgroup(
    WT_SESSION_IMPL *session, const char *name, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_TABLE *table;
	const char *cfg[] = { __wt_confdfl_colgroup_meta, config, NULL, NULL };
	const char *cgconf, *cgname, *tablename;
	char *filename, *namebuf;
	size_t namelen, tlen;
	int ret;

	cgconf = namebuf = NULL;
	tablename = name + strlen("colgroup:");
	cgname = strchr(tablename, ':');
	if (cgname == NULL) {
		__wt_errx(session, "Invalid colgroup name, "
		     "should be <table name>:<colgroup name>: %s", name);
		return (EINVAL);
	}

	tlen = (size_t)(cgname - tablename);
	if ((ret = __wt_schema_get_table(session,
	    tablename, tlen, &table)) != 0) {
		__wt_errx(session,
		    "Can't create a colgroup for a non-existent table: %.*s",
		    (int)tlen, tablename);
		return (ret);
	}

	++cgname;

	if (__wt_config_getones(session, config, "filename", &cval) == 0) {
		WT_RET(__wt_strndup(session, cval.str, cval.len, &namebuf));
		filename = namebuf;
	} else {
		namelen = strlen("filename=_.wtc0") + tlen + strlen(cgname);
		WT_RET(__wt_calloc_def(session, namelen, &namebuf));
		snprintf(namebuf, namelen, "filename=%.*s_%s.wtc",
		    (int)tlen, tablename, cgname);
		filename = namebuf + strlen("filename=");
		cfg[2] = namebuf;
	}

	WT_ERR(__wt_config_collapse(session, cfg, &cgconf));
	WT_ERR(__wt_schema_table_insert(session, name, cgconf));
	WT_ERR(__create_btree(session, filename, 1, config));

	WT_ERR(__wt_schema_open_colgroups(session, table));

err:	__wt_free(session, namebuf);
	__wt_free(session, cgconf);
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
		namelen = strlen("filename=_.wti0") + tlen + strlen(idxname);
		WT_RET(__wt_calloc_def(session, namelen, &namebuf));
		snprintf(namebuf, namelen, "filename=%.*s_%s.wti",
		    (int)tlen, tablename, idxname);
		filename = namebuf + strlen("filename=");
		cfg[2] = namebuf;
	}

	WT_ERR(__wt_config_collapse(session, cfg, &idxconf));
	WT_ERR(__wt_schema_table_insert(session, name, idxconf));
	WT_ERR(__create_btree(session, filename, 1, config));

err:	__wt_free(session, idxconf);
	__wt_free(session, namebuf);
	return (ret);
}

static int
__create_table(WT_SESSION_IMPL *session, const char *name, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_TABLE *table;
	const char *cfg[] = { __wt_confdfl_table_meta, config, NULL, NULL };
	const char *tableconf, *tablename;
	char *filename, *namebuf;
	size_t namelen;
	int ret;

	tableconf = namebuf = NULL;
	tablename = name + strlen("table:");

	if ((ret = __wt_schema_get_table(session,
	    tablename, strlen(tablename), &table)) == 0) {
		__wt_errx(session, "Table exists: %s", tablename);
		return (EEXIST);
	} else if (ret != WT_NOTFOUND)
		return (ret);

	if (__wt_config_getones(session, config, "filename", &cval) == 0) {
		WT_RET(__wt_strndup(session, cval.str, cval.len, &namebuf));
		filename = namebuf;
	} else {
		namelen = strlen("filename=_.wt0") + strlen(tablename);
		WT_RET(__wt_calloc_def(session, namelen, &namebuf));
		snprintf(namebuf, namelen, "filename=%s.wt", tablename);
		filename = namebuf + strlen("filename=");
		cfg[2] = namebuf;
	}

	WT_RET(__wt_config_collapse(session, cfg, &tableconf));
	WT_ERR(__wt_schema_table_insert(session, name, tableconf));
	WT_ERR(__create_btree(session, filename, 1, config));
	WT_ERR(__wt_schema_get_table(session,
	    tablename, strlen(tablename), &table));

err:	__wt_free(session, namebuf);
	__wt_free(session, tableconf);
	return (ret);
}

int
__wt_schema_create(
    WT_SESSION_IMPL *session, const char *name, const char *config)
{
	if (strncmp(name, "btree:", 6) == 0)
		return (__create_btree(session,
		    name + strlen("btree:"), 0, config));
	else if (strncmp(name, "colgroup:", 6) == 0)
		return (__create_colgroup(session, name, config));
	else if (strncmp(name, "index:", 6) == 0)
		return (__create_index(session, name, config));
	else if (strncmp(name, "schema:", 7) == 0)
		return (__create_btree(session,
		    name + strlen("schema:"), 1, config));
	else if (strncmp(name, "table:", 6) == 0)
		return (__create_table(session, name, config));
	else {
		__wt_errx(session, "Unknown object type: %s", name);
		return (EINVAL);
	}
}
