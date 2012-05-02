/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static const char *metafile_config = "key_format=S,value_format=S";

/*
 * __wt_open_metadata --
 *	Opens the metadata file, sets session->metafile.
 */
int
__wt_open_metadata(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	int tracking;
	const char *cfg[] = API_CONF_DEFAULTS(file, meta, metafile_config);
	const char *metaconf;

	if (session->metafile != NULL)
		return (0);

	WT_RET(__wt_config_collapse(session, cfg, &metaconf));

	/*
	 * Turn off tracking when creating the metadata file: this is always
	 * done before any other metadata operations and there is no going back.
	 */
	tracking = WT_META_TRACKING(session);
	if (tracking)
		__wt_meta_track_off(session, 0);
	WT_ERR(__wt_create_file(session, WT_METADATA_URI, 0, metaconf));
	session->metafile = session->btree;
err:	__wt_free(session, metaconf);
	if (tracking)
		WT_TRET(__wt_meta_track_on(session));
	return (ret);
}

/*
 * __wt_metadata_cursor --
 *	Opens a cursor on the metadata.
 */
int
__wt_metadata_cursor(
    WT_SESSION_IMPL *session, const char *config, WT_CURSOR **cursorp)
{
	const char *cfg[] = API_CONF_DEFAULTS(session, open_cursor, config);

	WT_RET(__wt_open_metadata(session));
	session->btree = session->metafile;
	return (__wt_curfile_create(session, NULL, cfg, cursorp));
}

/*
 * __wt_metadata_insert --
 *	Insert a row into the metadata
 */
int
__wt_metadata_insert(
    WT_SESSION_IMPL *session, const char *key, const char *value)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;

	if (WT_META_TRACKING(session))
		WT_RET(__wt_meta_track_insert(session, key));

	/* Save the caller's btree: the metadata cursor will overwrite it. */
	btree = session->btree;
	WT_RET(__wt_metadata_cursor(session, NULL, &cursor));
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	WT_TRET(cursor->insert(cursor));
	WT_TRET(cursor->close(cursor));

	/* Restore the caller's btree. */
	session->btree = btree;
	return (ret);
}

/*
 * __wt_metadata_update --
 *	Update a row in the metadata.
 */
int
__wt_metadata_update(
    WT_SESSION_IMPL *session, const char *key, const char *value)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;

	if (WT_META_TRACKING(session))
		WT_RET(__wt_meta_track_update(session, key));

	/* Save the caller's btree: the metadata cursor will overwrite it. */
	btree = session->btree;
	WT_RET(__wt_metadata_cursor(session, "overwrite", &cursor));
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	WT_TRET(cursor->insert(cursor));
	WT_TRET(cursor->close(cursor));

	/* Restore the caller's btree. */
	session->btree = btree;
	return (ret);
}

/*
 * __wt_metadata_remove --
 *	Removes a row from the metadata.
 */
int
__wt_metadata_remove(WT_SESSION_IMPL *session, const char *key)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;

	if (WT_META_TRACKING(session))
		WT_RET(__wt_meta_track_update(session, key));

	/* Save the caller's btree: the metadata cursor will overwrite it. */
	btree = session->btree;
	WT_RET(__wt_metadata_cursor(session, NULL, &cursor));
	cursor->set_key(cursor, key);
	WT_TRET(cursor->remove(cursor));
	WT_TRET(cursor->close(cursor));

	/* Restore the caller's btree. */
	session->btree = btree;
	return (ret);
}

/*
 * __wt_metadata_read --
 *	Reads and copies a row from the metadata.
 *	The caller is responsible for freeing the allocated memory.
 */
int
__wt_metadata_read(
    WT_SESSION_IMPL *session, const char *key, const char **valuep)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	const char *value;

	/* Save the caller's btree: the metadata cursor will overwrite it. */
	btree = session->btree;
	WT_RET(__wt_metadata_cursor(session, NULL, &cursor));
	cursor->set_key(cursor, key);
	WT_ERR(cursor->search(cursor));
	WT_ERR(cursor->get_value(cursor, &value));
	WT_ERR(__wt_strdup(session, value, valuep));

err:    WT_TRET(cursor->close(cursor));
	/* Restore the caller's btree. */
	session->btree = btree;
	return (ret);
}
