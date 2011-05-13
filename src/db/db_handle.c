/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_btree_config(BTREE *);

/*
 * __wt_connection_btree --
 *	BTREE constructor.
 */
int
__wt_connection_btree(CONNECTION *conn, BTREE **btreep)
{
	BTREE *btree;
	int ret;

	btree = NULL;

	/* Create the BTREE structure. */
	WT_ERR(__wt_calloc(&conn->default_session, 1, sizeof(BTREE), &btree));

	/* Connect everything together. */
	btree->conn = conn;

	/* Configure the BTREE and the BTREE. */
	WT_ERR(__wt_btree_config(btree));

	*btreep = btree;
	return (0);

err:	(void)__wt_btree_destroy(btree);
	return (ret);
}

/*
 * __wt_btree_config --
 *	Set configuration for a just-created BTREE handle.
 */
static int
__wt_btree_config(BTREE *btree)
{
	CONNECTION *conn;
	SESSION *session;

	conn = btree->conn;
	session = &conn->default_session;

	btree->btree_compare = __wt_bt_lex_compare;
	btree->root_page.addr = btree->free_addr = WT_ADDR_INVALID;

	TAILQ_INIT(&btree->freeqa);		/* Free queues */
	TAILQ_INIT(&btree->freeqs);

	/* Add to the connection's list. */
	__wt_lock(session, conn->mtx);
	TAILQ_INSERT_TAIL(&conn->dbqh, btree, q);
	++conn->dbqcnt;
	__wt_unlock(session, conn->mtx);

	WT_RET(__wt_stat_alloc_btree_stats(session, &btree->stats));
	WT_RET(__wt_stat_alloc_btree_file_stats(session, &btree->fstats));

	return (0);
}

/*
 * __wt_btree_destroy --
 *	BTREE handle destructor.
 */
int
__wt_btree_destroy(BTREE *btree)
{
	CONNECTION *conn;
	SESSION *session;
	int ret;

	conn = btree->conn;
	session = &conn->default_session;
	ret = 0;

	/* Check that there's something to close. */
	if (btree == NULL)
		return (0);

	__wt_free(session, btree->name);
	__wt_free(session, btree->config);

	if (btree->huffman_key != NULL) {
		/* Key and data may use the same table, only close it once. */
		if (btree->huffman_value == btree->huffman_key)
			btree->huffman_value = NULL;
		__wt_huffman_close(session, btree->huffman_key);
		btree->huffman_key = NULL;
	}
	if (btree->huffman_value != NULL) {
		__wt_huffman_close(session, btree->huffman_value);
		btree->huffman_value = NULL;
	}

	__wt_walk_end(session, &btree->evict_walk);

	__wt_free(session, btree->stats);
	__wt_free(session, btree->fstats);

	/* Remove from the connection's list. */
	__wt_lock(session, conn->mtx);
	TAILQ_REMOVE(&conn->dbqh, btree, q);
	--conn->dbqcnt;
	__wt_unlock(session, conn->mtx);

	/* Discard the BTREE object. */
	__wt_free(session, btree);

	return (ret);
}
