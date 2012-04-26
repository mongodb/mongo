/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static const char *schematab_config = "key_format=S,value_format=S";

/*
 * __wt_open_schema_table --
 *	Opens the schema table, sets session->schematab.
 */
int
__wt_open_schema_table(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	int tracking;
	const char *cfg[] = API_CONF_DEFAULTS(file, meta, schematab_config);
	const char *schemaconf;

	if (session->schematab != NULL)
		return (0);

	WT_RET(__wt_config_collapse(session, cfg, &schemaconf));

	/*
	 * Turn off tracking when creating the schema file: this is always done
	 * before any other schema operations and there is no going back.
	 */
	tracking = (session->schema_track != NULL);
	if (tracking)
		__wt_schema_table_track_off(session, 0);
	WT_ERR(__wt_create_file(session,
	    "file:" WT_SCHEMA_FILENAME,
	    "file:" WT_SCHEMA_FILENAME, 0, schemaconf));
	session->schematab = session->btree;
err:	__wt_free(session, schemaconf);
	if (tracking)
		WT_TRET(__wt_schema_table_track_on(session));
	return (ret);
}

/*
 * __wt_schema_table_cursor --
 *	Opens a cursor on the schema table.
 */
int
__wt_schema_table_cursor(
    WT_SESSION_IMPL *session, const char *config, WT_CURSOR **cursorp)
{
	const char *cfg[] = API_CONF_DEFAULTS(session, open_cursor, config);

	WT_RET(__wt_open_schema_table(session));
	session->btree = session->schematab;
	return (__wt_curfile_create(session, NULL, cfg, cursorp));
}

/*
 * __wt_schema_table_insert --
 *	Insert a row into the schema table.
 */
int
__wt_schema_table_insert(
    WT_SESSION_IMPL *session, const char *key, const char *value)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;

	if (session->schema_track != NULL)		/* Optional tracking */
		WT_RET(__wt_schema_table_track_insert(session, key));

	/* Save the caller's btree: the schema cursor will overwrite it. */
	btree = session->btree;
	WT_RET(__wt_schema_table_cursor(session, NULL, &cursor));
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	WT_TRET(cursor->insert(cursor));
	WT_TRET(cursor->close(cursor));

	/* Restore the caller's btree. */
	session->btree = btree;
	return (ret);
}

/*
 * __wt_schema_table_update --
 *	Update a row in the schema table.
 */
int
__wt_schema_table_update(
    WT_SESSION_IMPL *session, const char *key, const char *value)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;

	if (session->schema_track != NULL)		/* Optional tracking */
		WT_RET(__wt_schema_table_track_update(session, key));

	/* Save the caller's btree: the schema cursor will overwrite it. */
	btree = session->btree;
	WT_RET(__wt_schema_table_cursor(session, "overwrite", &cursor));
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	WT_TRET(cursor->insert(cursor));
	WT_TRET(cursor->close(cursor));

	/* Restore the caller's btree. */
	session->btree = btree;
	return (ret);
}

/*
 * __wt_schema_table_remove --
 *	Removes a row from the schema table.
 */
int
__wt_schema_table_remove(WT_SESSION_IMPL *session, const char *key)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;

	if (session->schema_track != NULL)		/* Optional tracking */
		WT_RET(__wt_schema_table_track_update(session, key));

	/* Save the caller's btree: the schema cursor will overwrite it. */
	btree = session->btree;
	WT_RET(__wt_schema_table_cursor(session, NULL, &cursor));
	cursor->set_key(cursor, key);
	WT_TRET(cursor->remove(cursor));
	WT_TRET(cursor->close(cursor));

	/* Restore the caller's btree. */
	session->btree = btree;
	return (ret);
}

/*
 * __wt_schema_table_read --
 *	Reads and copies a row from the schema table.
 *	The caller is responsible for freeing the allocated memory.
 */
int
__wt_schema_table_read(
    WT_SESSION_IMPL *session, const char *key, const char **valuep)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	const char *value;

	/* Save the caller's btree: the schema cursor will overwrite it. */
	btree = session->btree;
	WT_RET(__wt_schema_table_cursor(session, NULL, &cursor));
	cursor->set_key(cursor, key);
	WT_ERR(cursor->search(cursor));
	WT_ERR(cursor->get_value(cursor, &value));
	WT_ERR(__wt_strdup(session, value, valuep));

err:    WT_TRET(cursor->close(cursor));
	/* Restore the caller's btree. */
	session->btree = btree;
	return (ret);
}
