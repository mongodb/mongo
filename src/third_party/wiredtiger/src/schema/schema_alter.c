/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __alter_file --
 *	Alter a file.
 */
static int
__alter_file(WT_SESSION_IMPL *session, const char *uri, const char *newcfg[])
{
	WT_DECL_RET;
	const char *cfg[4], *filename;
	char *config, *newconfig;

	filename = uri;
	newconfig = NULL;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		return (__wt_unexpected_object_type(session, uri, "file:"));

	/* Find the URI */
	WT_RET(__wt_metadata_search(session, uri, &config));

	WT_ASSERT(session, newcfg[0] != NULL);
	/*
	 * Start with the base configuration because collapse is like
	 * a projection and if we are reading older metadata, it may not
	 * have all the components.
	 */
	cfg[0] = WT_CONFIG_BASE(session, file_meta);
	cfg[1] = config;
	cfg[2] = newcfg[0];
	cfg[3] = NULL;
	WT_ERR(__wt_config_collapse(session, cfg, &newconfig));
	/*
	 * Only rewrite if there are changes.
	 */
	if (strcmp(config, newconfig) != 0)
		WT_ERR(__wt_metadata_update(session, uri, newconfig));
	else
		WT_STAT_CONN_INCR(session, session_table_alter_skip);

err:	__wt_free(session, config);
	__wt_free(session, newconfig);
	return (ret);
}

/*
 * __alter_colgroup --
 *	WT_SESSION::alter for a colgroup.
 */
static int
__alter_colgroup(
    WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
	WT_COLGROUP *colgroup;
	WT_DECL_RET;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_TABLE));

	/* If we can get the colgroup, perform any potential alterations. */
	if ((ret = __wt_schema_get_colgroup(
	    session, uri, false, NULL, &colgroup)) == 0)
		WT_TRET(__wt_schema_alter(session, colgroup->source, cfg));

	return (ret);
}

/*
 * __alter_index --
 *	WT_SESSION::alter for an index.
 */
static int
__alter_index(
    WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
	WT_INDEX *idx;
	WT_DECL_RET;

	/* If we can get the index, perform any potential alterations. */
	if ((ret = __wt_schema_get_index(
	    session, uri, false, NULL, &idx)) == 0)
		WT_TRET(__wt_schema_alter(session, idx->source, cfg));

	return (ret);
}

/*
 * __alter_table --
 *	WT_SESSION::alter for a table.
 */
static int
__alter_table(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
	WT_COLGROUP *colgroup;
	WT_DECL_RET;
	WT_TABLE *table;
	const char *name;
	u_int i;

	name = uri;
	(void)WT_PREFIX_SKIP(name, "table:");

	WT_RET(__wt_schema_get_table(
	    session, name, strlen(name), true, &table));

	/*
	 * Alter the column groups only if we are using the default
	 * column group.  Otherwise the user should alter each
	 * index or column group explicitly.
	 */
	if (table->ncolgroups == 0)
		for (i = 0; i < WT_COLGROUPS(table); i++) {
			if ((colgroup = table->cgroups[i]) == NULL)
				continue;
			/*
			 * Alter the column group before updating the metadata
			 * to avoid the metadata for the table becoming
			 * inconsistent if we can't get exclusive access.
			 */
			WT_ERR(__wt_schema_alter(
			    session, colgroup->source, cfg));
		}
err:	__wt_schema_release_table(session, table);
	return (ret);
}

/*
 * __wt_schema_alter --
 *	Process a WT_SESSION::alter operation for all supported types.
 */
int
__wt_schema_alter(WT_SESSION_IMPL *session, const char *uri, const char *cfg[])
{
	WT_DATA_SOURCE *dsrc;
	WT_DECL_RET;

	WT_RET(__wt_meta_track_on(session));

	/* Paranoia: clear any handle from our caller. */
	session->dhandle = NULL;

	if (WT_PREFIX_MATCH(uri, "colgroup:"))
		ret = __alter_colgroup(session, uri, cfg);
	else if (WT_PREFIX_MATCH(uri, "file:"))
		ret = __alter_file(session, uri, cfg);
	else if (WT_PREFIX_MATCH(uri, "index:"))
		ret = __alter_index(session, uri, cfg);
	else if (WT_PREFIX_MATCH(uri, "lsm:"))
		ret = __wt_lsm_tree_alter(session, uri, cfg);
	else if (WT_PREFIX_MATCH(uri, "table:"))
		ret = __alter_table(session, uri, cfg);
	else if ((dsrc = __wt_schema_get_source(session, uri)) != NULL)
		ret = dsrc->alter == NULL ?
		    __wt_object_unsupported(session, uri) :
		    dsrc->alter(dsrc,
		    &session->iface, uri, (WT_CONFIG_ARG *)cfg);
	else
		ret = __wt_bad_object_type(session, uri);

	/*
	 * Map WT_NOTFOUND to ENOENT, based on the assumption WT_NOTFOUND means
	 * there was no metadata entry.
	 */
	if (ret == WT_NOTFOUND)
		ret = ENOENT;

	/* Bump the schema generation so that stale data is ignored. */
	(void)__wt_gen_next(session, WT_GEN_SCHEMA);

	WT_TRET(__wt_meta_track_off(session, true, ret != 0));

	return (ret);
}
