/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

#define	SCHEMATAB_NAME	"__schema.wt"
static const char *schematab_config = "key_format=S,value_format=S";

static inline int
__open_schema_table(WT_SESSION_IMPL *session)
{
	WT_BTREE_SESSION *btree_session;
	const char *cfg[] = API_CONF_DEFAULTS(btree, meta, schematab_config);
	const char *schemaconf;
	int ret;

	if (session->schematab != NULL)
		return (0);

	WT_RET(__wt_config_collapse(session, cfg, &schemaconf));
	WT_ERR(__wt_schema_create(session,
	    "schema:" SCHEMATAB_NAME, schemaconf));
	WT_ERR(__wt_session_get_btree(session,
	    SCHEMATAB_NAME, strlen(SCHEMATAB_NAME), &btree_session));
	session->schematab = btree_session->btree;
err:	__wt_free(session, schemaconf);
	return (ret);
}

int
__wt_schema_table_cursor(WT_SESSION_IMPL *session, WT_CURSOR **cursorp)
{
	WT_RET(__open_schema_table(session));
	session->btree = session->schematab;
	return (__wt_curbtree_create(session, 0, NULL, cursorp));
}

int
__wt_schema_table_insert(
    WT_SESSION_IMPL *session, const char *key, const char *value)
{
	WT_CURSOR *cursor;
	int ret;

	WT_RET(__wt_schema_table_cursor(session, &cursor));
	ret = 0;
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	WT_TRET(cursor->insert(cursor));
	WT_TRET(cursor->close(cursor, NULL));
	return (ret);
}
