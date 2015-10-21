/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __metadata_turtle --
 *	Return if a key's value should be taken from the turtle file.
 */
static bool
__metadata_turtle(const char *key)
{
	switch (key[0]) {
	case 'f':
		if (strcmp(key, WT_METAFILE_URI) == 0)
			return (true);
		break;
	case 'W':
		if (strcmp(key, "WiredTiger version") == 0)
			return (true);
		if (strcmp(key, "WiredTiger version string") == 0)
			return (true);
		break;
	}
	return (false);
}

/*
 * __wt_metadata_cursor_open --
 *	Opens a cursor on the metadata.
 */
int
__wt_metadata_cursor_open(
    WT_SESSION_IMPL *session, const char *config, WT_CURSOR **cursorp)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	const char *open_cursor_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_open_cursor), config, NULL };

	WT_WITHOUT_DHANDLE(session, ret = __wt_open_cursor(
	    session, WT_METAFILE_URI, NULL, open_cursor_cfg, cursorp));
	WT_RET(ret);

	/* 
	 * Set special flags for the metadata file: eviction (the metadata file
	 * is in-memory and never evicted), logging (the metadata file is always
	 * logged if possible).
	 *
	 * Test flags before setting them so updates can't race in subsequent
	 * opens (the first update is safe because it's single-threaded from
	 * wiredtiger_open).
	 */
	btree = ((WT_CURSOR_BTREE *)(*cursorp))->btree;
	if (!F_ISSET(btree, WT_BTREE_IN_MEMORY))
		F_SET(btree, WT_BTREE_IN_MEMORY);
	if (!F_ISSET(btree, WT_BTREE_NO_EVICTION))
		F_SET(btree, WT_BTREE_NO_EVICTION);
	if (F_ISSET(btree, WT_BTREE_NO_LOGGING))
		F_CLR(btree, WT_BTREE_NO_LOGGING);

	return (0);
}

/*
 * __wt_metadata_cursor --
 *	Opens the session's cached metadata cursor.
 */
int
__wt_metadata_cursor(WT_SESSION_IMPL *session, WT_CURSOR **cursorp)
{
	if (session->meta_cursor == NULL)
		WT_RET(__wt_metadata_cursor_open(
		    session, NULL, &session->meta_cursor));
	if (cursorp != NULL)
		*cursorp = session->meta_cursor;
	return (0);
}

/*
 * __wt_metadata_insert --
 *	Insert a row into the metadata.
 */
int
__wt_metadata_insert(
    WT_SESSION_IMPL *session, const char *key, const char *value)
{
	WT_CURSOR *cursor;

	WT_RET(__wt_verbose(session, WT_VERB_METADATA,
	    "Insert: key: %s, value: %s, tracking: %s, %s" "turtle",
	    key, value, WT_META_TRACKING(session) ? "true" : "false",
	    __metadata_turtle(key) ? "" : "not "));

	if (__metadata_turtle(key))
		WT_RET_MSG(session, EINVAL,
		    "%s: insert not supported on the turtle file", key);

	WT_RET(__wt_metadata_cursor(session, &cursor));
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	WT_RET(cursor->insert(cursor));
	if (WT_META_TRACKING(session))
		WT_RET(__wt_meta_track_insert(session, key));
	return (cursor->reset(cursor));
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

	WT_RET(__wt_verbose(session, WT_VERB_METADATA,
	    "Update: key: %s, value: %s, tracking: %s, %s" "turtle",
	    key, value, WT_META_TRACKING(session) ? "true" : "false",
	    __metadata_turtle(key) ? "" : "not "));

	if (__metadata_turtle(key)) {
		WT_WITH_TURTLE_LOCK(session,
		    ret = __wt_turtle_update(session, key, value));
		return (ret);
	}

	if (WT_META_TRACKING(session))
		WT_RET(__wt_meta_track_update(session, key));

	WT_RET(__wt_metadata_cursor(session, &cursor));
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	WT_RET(cursor->insert(cursor));
	return (cursor->reset(cursor));
}

/*
 * __wt_metadata_remove --
 *	Remove a row from the metadata.
 */
int
__wt_metadata_remove(WT_SESSION_IMPL *session, const char *key)
{
	WT_CURSOR *cursor;

	WT_RET(__wt_verbose(session, WT_VERB_METADATA,
	    "Remove: key: %s, tracking: %s, %s" "turtle",
	    key, WT_META_TRACKING(session) ? "true" : "false",
	    __metadata_turtle(key) ? "" : "not "));

	if (__metadata_turtle(key))
		WT_RET_MSG(session, EINVAL,
		    "%s: remove not supported on the turtle file", key);

	WT_RET(__wt_metadata_cursor(session, &cursor));
	cursor->set_key(cursor, key);
	WT_RET(cursor->search(cursor));
	if (WT_META_TRACKING(session))
		WT_RET(__wt_meta_track_update(session, key));
	cursor->set_key(cursor, key);
	WT_RET(cursor->remove(cursor));
	return (cursor->reset(cursor));
}

/*
 * __wt_metadata_search --
 *	Return a copied row from the metadata.
 *	The caller is responsible for freeing the allocated memory.
 */
int
__wt_metadata_search(WT_SESSION_IMPL *session, const char *key, char **valuep)
{
	WT_CURSOR *cursor;
	const char *value;

	*valuep = NULL;

	WT_RET(__wt_verbose(session, WT_VERB_METADATA,
	    "Search: key: %s, tracking: %s, %s" "turtle",
	    key, WT_META_TRACKING(session) ? "true" : "false",
	    __metadata_turtle(key) ? "" : "not "));

	if (__metadata_turtle(key))
		return (__wt_turtle_read(session, key, valuep));

	WT_RET(__wt_metadata_cursor(session, &cursor));
	cursor->set_key(cursor, key);
	WT_RET(cursor->search(cursor));
	WT_RET(cursor->get_value(cursor, &value));
	WT_RET(__wt_strdup(session, value, valuep));
	return (cursor->reset(cursor));
}
