/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_db_idb_open(DB *, const char *, mode_t, uint32_t);

/*
 * __wt_db_open --
 *	Open a DB handle.
 */
int
__wt_db_open(WT_TOC *toc, const char *name, mode_t mode, uint32_t flags)
{
	DB *db;
	ENV *env;

	env = toc->env;
	db = toc->db;

	WT_STAT_INCR(env->ienv->stats, DATABASE_OPEN);

	/* Initialize the IDB structure. */
	WT_RET(__wt_db_idb_open(db, name, mode, flags));

	/* Open the underlying Btree. */
	WT_RET(__wt_bt_open(toc, LF_ISSET(WT_CREATE) ? 1 : 0));

	/* Turn on the methods that require open. */
	__wt_methods_db_open_transition(db);

	return (0);
}

/*
 * __wt_db_idb_open --
 *	Routine to intialize any IDB values based on a DB value during open.
 */
static int
__wt_db_idb_open(DB *db, const char *name, mode_t mode, uint32_t flags)
{
	ENV *env;
	IENV *ienv;
	IDB *idb;

	env = db->env;
	ienv = env->ienv;
	idb = db->idb;

	WT_RET(__wt_strdup(env, name, &idb->name));
	idb->mode = mode;

	__wt_lock(env, ienv->mtx);
	idb->file_id = ++ienv->next_file_id;
	__wt_unlock(env, ienv->mtx);

	/*
	 * XXX
	 * Initialize the root WT_REF/WT_OFF pair to point to the start of
	 * the file.  This is all wrong, and we'll get the information from
	 * somewhere else, eventually.
	 */
	WT_CLEAR(idb->root_page);
	idb->root_page.state = WT_REF_DISK;
	WT_CLEAR(idb->root_off);
	idb->root_off.addr = 0;
	idb->root_off.size = 0;

	if (LF_ISSET(WT_RDONLY))
		F_SET(idb, WT_RDONLY);

	return (0);
}

/*
 * __wt_db_close --
 *	Db.close method (DB close & handle destructor).
 */
int
__wt_db_close(WT_TOC *toc, uint32_t flags)
{
	DB *db;
	int ret;

	db = toc->db;
	ret = 0;

	/* Flush the underlying Btree. */
	if (!LF_ISSET(WT_NOWRITE))
		WT_TRET(__wt_bt_sync(toc));

	/* Close the underlying Btree. */
	ret = __wt_bt_close(toc);

	WT_TRET(__wt_db_destroy(db));

	return (ret);
}
