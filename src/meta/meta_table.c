/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __metadata_turtle --
 *	Return if a key's value should be taken from the turtle file.
 */
static int
__metadata_turtle(const char *key)
{
	switch (key[0]) {
	case 'f':
		if (strcmp(key, WT_METADATA_URI) == 0)
			return (1);
		break;
	case 'W':
		if (strcmp(key, "WiredTiger version") == 0)
			return (1);
		if (strcmp(key, "WiredTiger version string") == 0)
			return (1);
		break;
	}
	return (0);
}

/*
 * __wt_metadata_open --
 *	Opens the metadata file, sets session->metafile.
 */
int
__wt_metadata_open(WT_SESSION_IMPL *session)
{
	if (session->metafile != NULL)
		return (0);

	WT_RET(__wt_session_get_btree(
	    session, WT_METADATA_URI, NULL, 0));

	session->metafile = session->btree;
	WT_ASSERT(session, session->metafile != NULL);

	/* The metafile doesn't need to stay locked -- release it. */
	return (__wt_session_release_btree(session));
}

/*
 * __wt_metadata_load_backup --
 *	Load the contents of any hot backup file.
 */
int
__wt_metadata_load_backup(WT_SESSION_IMPL *session)
{
	FILE *fp;
	WT_DECL_RET;
	size_t keylen, keybuflen, valuelen, valuebuflen;
	char *key, *value;
	const char *path;

	fp = NULL;
	key = value = NULL;
	path = NULL;

	/* Look for a hot backup file: if we find it, load it. */
	WT_RET(__wt_filename(session, WT_METADATA_BACKUP, &path));
	if ((fp = fopen(path, "r")) == NULL) {
		__wt_free(session, path);
		return (0);
	}

	/* Read line pairs and load them into the metadata file. */
	key = value = NULL;
	keybuflen = valuebuflen = 0;
	for (;;) {
		WT_ERR(__wt_getline(session, &key, &keybuflen, &keylen, fp));
		if (keylen == 0)
			break;
		WT_ERR(
		    __wt_getline(session, &value, &valuebuflen, &valuelen, fp));
		if (valuelen == 0)
			WT_ERR(__wt_illegal_value(session, WT_METADATA_BACKUP));
		WT_ERR(__wt_metadata_update(session, key, value));
	}

	/* Remove the hot backup file, it's only read (successfully) once. */
	WT_ERR(__wt_remove(session, WT_METADATA_BACKUP));

err:	if (fp != NULL)
		WT_TRET(fclose(fp));
	if (key != NULL)
		__wt_free(session, key);
	if (value != NULL)
		__wt_free(session, value);
	if (path != NULL)
		__wt_free(session, path);
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
	WT_BTREE *saved_btree;
	WT_DECL_RET;
	const char *cfg[] = API_CONF_DEFAULTS(session, open_cursor, config);

	saved_btree = session->btree;
	WT_ERR(__wt_metadata_open(session));

	session->btree = session->metafile;
	WT_ERR(__wt_session_lock_btree(session, 0));
	ret = __wt_curfile_create(session, NULL, cfg, cursorp);

	/* Restore the caller's btree. */
err:	session->btree = saved_btree;
	return (ret);
}

/*
 * __wt_metadata_insert --
 *	Insert a row into the metadata
 */
int
__wt_metadata_insert(
    WT_SESSION_IMPL *session, const char *key, const char *value)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;

	if (__metadata_turtle(key))
		WT_RET_MSG(session, EINVAL,
		    "%s: insert not supported on the turtle file", key);

	WT_ERR(__wt_metadata_cursor(session, NULL, &cursor));
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	WT_TRET(cursor->insert(cursor));
	if (ret == 0 && WT_META_TRACKING(session))
		ret = __wt_meta_track_insert(session, key);
	WT_TRET(cursor->close(cursor));

err:	return (ret);
}

/*
 * __wt_metadata_update --
 *	Update a row in the metadata.
 */
int
__wt_metadata_update(
    WT_SESSION_IMPL *session, const char *key, const char *value)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;

	if (__metadata_turtle(key))
		return (__wt_meta_turtle_update(session, key, value));

	if (WT_META_TRACKING(session))
		WT_RET(__wt_meta_track_update(session, key));

	WT_RET(__wt_metadata_cursor(session, "overwrite", &cursor));
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	WT_TRET(cursor->insert(cursor));
	WT_TRET(cursor->close(cursor));

	return (ret);
}

/*
 * __wt_metadata_remove --
 *	Removes a row from the metadata.
 */
int
__wt_metadata_remove(WT_SESSION_IMPL *session, const char *key)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;

	if (__metadata_turtle(key))
		WT_RET_MSG(session, EINVAL,
		    "%s: remove not supported on the turtle file", key);

	WT_RET(__wt_metadata_cursor(session, NULL, &cursor));
	cursor->set_key(cursor, key);
	WT_TRET(cursor->search(cursor));
	if (ret == 0) {
		if (WT_META_TRACKING(session))
			WT_TRET(__wt_meta_track_update(session, key));
		WT_TRET(cursor->remove(cursor));
	}
	WT_TRET(cursor->close(cursor));

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
	WT_CURSOR *cursor;
	WT_DECL_RET;
	const char *value;

	if (__metadata_turtle(key))
		return (__wt_meta_turtle_read(session, key, valuep));

	WT_RET(__wt_metadata_cursor(session, NULL, &cursor));
	cursor->set_key(cursor, key);
	WT_ERR(cursor->search(cursor));
	WT_ERR(cursor->get_value(cursor, &value));
	WT_ERR(__wt_strdup(session, value, valuep));

err:	WT_TRET(cursor->close(cursor));
	return (ret);
}
