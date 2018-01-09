/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_alter --
 *	Alter a file.
 */
int
__wt_alter(WT_SESSION_IMPL *session, const char *newcfg[])
{
	WT_DECL_RET;
	const char *cfg[4], *filename, *uri;
	char *config, *newconfig;

	uri = session->dhandle->name;
	WT_RET(__wt_meta_track_on(session));

	/*
	 * We know that we have exclusive access to the file.  So it will be
	 * closed after we're done with it and the next open will see the
	 * updated metadata.
	 */
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
	/*
	 * Map WT_NOTFOUND to ENOENT, based on the assumption WT_NOTFOUND means
	 * there was no metadata entry.
	 */
	if (ret == WT_NOTFOUND)
		ret = ENOENT;

	WT_TRET(__wt_meta_track_off(session, true, ret != 0));

	return (ret);
}
