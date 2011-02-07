/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_db_config(DB *);
static int __wt_idb_config(DB *);
static int __wt_idb_destroy(DB *);

/*
 * __wt_env_db --
 *	DB constructor.
 */
int
__wt_env_db(ENV *env, DB **dbp)
{
	DB *db;
	BTREE *btree;
	int ret;

	db = NULL;
	btree = NULL;

	/* Create the DB and BTREE structures. */
	WT_ERR(__wt_calloc(env, 1, sizeof(DB), &db));
	WT_ERR(__wt_calloc(env, 1, sizeof(BTREE), &btree));

	/* Connect everything together. */
	db->btree = btree;
	btree->db = db;
	db->env = env;

	/* Configure the DB and the BTREE. */
	WT_ERR(__wt_db_config(db));
	WT_ERR(__wt_idb_config(db));

	*dbp = db;
	return (0);

err:	(void)__wt_db_destroy(db);
	return (ret);
}

/*
 * __wt_db_config --
 *	Set configuration for a just-created DB handle.
 */
static int
__wt_db_config(DB *db)
{
	__wt_methods_db_config_default(db);
	__wt_methods_db_lockout(db);
	__wt_methods_db_init_transition(db);

	return (0);
}

/*
 * __wt_idb_config --
 *	Set configuration for a just-created BTREE handle.
 */
static int
__wt_idb_config(DB *db)
{
	ENV *env;
	BTREE *btree;
	IENV *ienv;

	env = db->env;
	btree = db->btree;
	ienv = env->ienv;

	btree->db = db;
	btree->root_page.addr = btree->free_addr = WT_ADDR_INVALID;

	TAILQ_INIT(&btree->freeqa);		/* Free queues */
	TAILQ_INIT(&btree->freeqs);

	__wt_lock(env, ienv->mtx);		/* Add to the ENV's list */
	TAILQ_INSERT_TAIL(&ienv->dbqh, btree, q);
	++ienv->dbqcnt;
	__wt_unlock(env, ienv->mtx);

	WT_RET(__wt_stat_alloc_btree_handle_stats(env, &btree->stats));
	WT_RET(__wt_stat_alloc_btree_file_stats(env, &btree->fstats));

	return (0);
}

/*
 * __wt_db_destroy --
 *	DB handle destructor.
 */
int
__wt_db_destroy(DB *db)
{
	ENV *env;
	int ret;

	env = db->env;

	/* Discard the underlying BTREE object. */
	ret = __wt_idb_destroy(db);

	/* Discard the DB object. */
	__wt_free(env, db, sizeof(DB));

	return (ret);
}

/*
 * __wt_idb_destroy --
 *	BTREE handle destructor.
 */
static int
__wt_idb_destroy(DB *db)
{
	ENV *env;
	BTREE *btree;
	IENV *ienv;
	int ret;

	env = db->env;
	btree = db->btree;
	ienv = env->ienv;
	ret = 0;

	/* Check that there's something to close. */
	if (btree == NULL)
		return (0);

	/* Diagnostic check: check flags against approved list. */
	WT_ENV_FCHK_RET(env, "Db.close", btree->flags, WT_APIMASK_IDB, ret);

	__wt_free(env, btree->name, 0);

	if (btree->huffman_key != NULL) {
		/* Key and data may use the same table, only close it once. */
		if (btree->huffman_data == btree->huffman_key)
			btree->huffman_data = NULL;
		__wt_huffman_close(env, btree->huffman_key);
		btree->huffman_key = NULL;
	}
	if (btree->huffman_data != NULL) {
		__wt_huffman_close(env, btree->huffman_data);
		btree->huffman_data = NULL;
	}

	__wt_walk_end(env, &btree->evict_walk);

	__wt_free(env, btree->stats, 0);
	__wt_free(env, btree->fstats, 0);

	__wt_lock(env, ienv->mtx);		/* Delete from the ENV's list */
	TAILQ_REMOVE(&ienv->dbqh, btree, q);
	--ienv->dbqcnt;
	__wt_unlock(env, ienv->mtx);

	__wt_free(env, btree, sizeof(BTREE));
	db->btree = NULL;
	return (ret);
}

int
__wt_db_lockout_err(DB *db)
{
	__wt_api_db_errx(db,
	    "This Db handle has failed for some reason, and can no longer "
	    "be used; the only method permitted on it is Db.close which "
	    "discards the handle permanently");
	return (WT_ERROR);
}

int
__wt_db_lockout_open(DB *db)
{
	__wt_api_db_errx(db,
	    "This method may not be called until after the Db.open method has "
	    "been called");
	return (WT_ERROR);
}
