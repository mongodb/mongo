/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_config_upgrade --
 *	Upgrade a configuration string by appended the replacement version.
 */
int
__wt_config_upgrade(WT_SESSION_IMPL *session, WT_ITEM *buf)
{
	WT_CONFIG_ITEM v;
	const char *config;

	config = buf->data;

	/*
	 * wiredtiger_open:
	 *	lsm_merge=boolean -> lsm_manager=(merge=boolean)
	 */
	if (__wt_config_getones(
	    session, config, "lsm_merge", &v) != WT_NOTFOUND)
		WT_RET(__wt_buf_catfmt(session, buf,
		    ",lsm_manager=(merge=%s)", v.val ? "true" : "false"));

	return (0);
}
