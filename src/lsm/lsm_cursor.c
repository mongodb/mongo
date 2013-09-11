/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define	WT_FORALL_CURSORS(clsm, c, i)					\
	for ((i) = (clsm)->nchunks; (i) > 0;)				\
		if (((c) = (clsm)->cursors[--i]) != NULL)

#define	WT_LSM_CURCMP(s, lsm_tree, c1, c2, cmp)				\
	WT_LEX_CMP(s, (lsm_tree)->collator, &(c1)->key, &(c2)->key, cmp)

/*
 * LSM API enter: check that the cursor is in sync with the tree.
 */
#define	WT_LSM_ENTER(clsm, cursor, session, n)				\
	clsm = (WT_CURSOR_LSM *)cursor;					\
	CURSOR_API_CALL(cursor, session, n, NULL);			\
	WT_ERR(__clsm_enter(clsm, 0))

#define	WT_LSM_UPDATE_ENTER(clsm, cursor, session, n)			\
	clsm = (WT_CURSOR_LSM *)cursor;					\
	CURSOR_UPDATE_API_CALL(cursor, session, n, NULL);		\
	WT_ERR(__clsm_enter(clsm, 1))

#define	WT_LSM_UPDATE_LEAVE(clsm, session, ret)				\
	CURSOR_UPDATE_API_END(session, ret);				\

static int __clsm_open_cursors(WT_CURSOR_LSM *, int, u_int, uint32_t);
static int __clsm_search(WT_CURSOR *);

static inline int
__clsm_enter(WT_CURSOR_LSM *clsm, int update)
{
	WT_LSM_CHUNK *chunk;
	WT_SESSION_IMPL *session;
	uint64_t *txnid_maxp;
	uint64_t id, myid, snap_min;

	session = (WT_SESSION_IMPL *)clsm->iface.session;

	/* Merge cursors never update. */
	if (F_ISSET(clsm, WT_CLSM_MERGE))
		return (0);

	for (;;) {
		/* Update the maximum transaction ID in the primary chunk. */
		if (update && (chunk = clsm->primary_chunk) != NULL) {
			WT_RET(__wt_txn_autocommit_check(session));
			for (id = chunk->txnid_max, myid = session->txn.id;
			    !TXNID_LE(myid, id);
			    id = chunk->txnid_max)
				(void)WT_ATOMIC_CAS(chunk->txnid_max, id, myid);
		}

		/*
		 * Figure out how many updates are required for snapshot
		 * isolation.
		 *
		 * This is not a normal visibility check on the maximum
		 * transaction ID in each chunk: any transaction ID that
		 * overlaps with our snapshot is a potential conflict.
		 */
		clsm->nupdates = 1;
		if (session->txn.isolation == TXN_ISO_SNAPSHOT &&
		    F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT)) {
			snap_min = session->txn.snap_min;
			for (txnid_maxp = &clsm->txnid_max[clsm->nchunks - 2];
			    clsm->nupdates < clsm->nchunks;
			    clsm->nupdates++, txnid_maxp--)
				if (TXNID_LT(*txnid_maxp, snap_min))
					break;
		}

		/*
		 * Stop when we are up-to-date, as long as this is:
		 *   - a snapshot isolation update and the cursor is set up for
		 *     that;
		 *   - an update operation with a primary chunk, or
		 *   - a read operation and the cursor is open for reading.
		 */
		if (clsm->dsk_gen == clsm->lsm_tree->dsk_gen &&
		    (!update || session->txn.isolation != TXN_ISO_SNAPSHOT ||
		    F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT)) &&
		    ((update && clsm->primary_chunk != NULL) ||
		    (!update && F_ISSET(clsm, WT_CLSM_OPEN_READ))))
			break;

		WT_RET(__clsm_open_cursors(clsm, update, 0, 0));
	}

	return (0);
}

/*
 * TODO: use something other than an empty value as a tombstone: we need
 * to support empty values from the application.
 */
static const WT_ITEM __lsm_tombstone = { "", 0, 0, NULL, 0 };

#define	WT_LSM_NEEDVALUE(c) do {					\
	WT_CURSOR_NEEDVALUE(c);						\
	if (__clsm_deleted((WT_CURSOR_LSM *)(c), &(c)->value))		\
		WT_ERR_MSG((WT_SESSION_IMPL *)cursor->session, EINVAL,	\
		    "LSM does not yet support zero-length data items");	\
} while (0)

/*
 * __clsm_deleted --
 *	Check whether the current value is a tombstone.
 */
static inline int
__clsm_deleted(WT_CURSOR_LSM *clsm, WT_ITEM *item)
{
	return (!F_ISSET(clsm, WT_CLSM_MINOR_MERGE) && item->size == 0);
}

/*
 * __clsm_close_cursors --
 *	Close any btree cursors that are not needed.
 */
static int
__clsm_close_cursors(WT_CURSOR_LSM *clsm, u_int ngood)
{
	WT_BLOOM *bloom;
	WT_CURSOR *c;
	u_int i;

	if (clsm->cursors == NULL || clsm->nchunks == 0)
		return (0);

	/*
	 * Walk the cursors, closing any we don't need.  Note that the exit
	 * condition here is special, don't use WT_FORALL_CURSORS, and be
	 * careful with unsigned integer wrapping.
	 */
	for (i = clsm->nchunks; i > ngood; ) {
		if ((c = (clsm)->cursors[--i]) != NULL) {
			clsm->cursors[i] = NULL;
			WT_RET(c->close(c));
		}
		if ((bloom = clsm->blooms[i]) != NULL) {
			clsm->blooms[i] = NULL;
			WT_RET(__wt_bloom_close(bloom));
		}
	}

	return (0);
}

/*
 * __clsm_open_cursors --
 *	Open cursors for the current set of files.
 */
static int
__clsm_open_cursors(
    WT_CURSOR_LSM *clsm, int update, u_int start_chunk, uint32_t start_id)
{
	WT_CURSOR *c, **cp, *primary;
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;
	WT_TXN *txn;
	const char *checkpoint, *ckpt_cfg[3];
	uint64_t saved_gen;
	u_int i, nchunks, ngood;
	int locked;

	c = &clsm->iface;
	session = (WT_SESSION_IMPL *)c->session;
	txn = &session->txn;
	lsm_tree = clsm->lsm_tree;
	chunk = NULL;

	ckpt_cfg[0] = WT_CONFIG_BASE(session, session_open_cursor);
	ckpt_cfg[1] = "checkpoint=WiredTigerCheckpoint,raw";
	ckpt_cfg[2] = NULL;

	/* Copy the key, so we don't lose the cursor position. */
	if (F_ISSET(c, WT_CURSTD_KEY_INT) && !WT_DATA_IN_ITEM(&c->key))
		WT_RET(__wt_buf_set(
		    session, &c->key, c->key.data, c->key.size));

	F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);

	if (update) {
		/*
		 * If this is the first update in this cursor, check if a new
		 * in-memory chunk is needed.
		 */
		if (clsm->primary_chunk == NULL) {
			WT_RET(__wt_writelock(session, lsm_tree->rwlock));
			if (clsm->dsk_gen == lsm_tree->dsk_gen)
				WT_WITH_SCHEMA_LOCK(session, ret =
				    __wt_lsm_tree_switch(session, lsm_tree));
			WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));
			WT_RET(ret);
		}
		if (txn->isolation == TXN_ISO_SNAPSHOT)
			F_SET(clsm, WT_CLSM_OPEN_SNAPSHOT);
	} else
		F_SET(clsm, WT_CLSM_OPEN_READ);

	WT_RET(__wt_readlock(session, lsm_tree->rwlock));
	locked = 1;
retry:
	F_SET(session, WT_SESSION_NO_CACHE_CHECK);

	/* Merge cursors have already figured out how many chunks they need. */
	if (F_ISSET(clsm, WT_CLSM_MERGE)) {
		nchunks = clsm->nchunks;
		ngood = 0;

		/*
		 * We may have raced with another merge completing.  Check that
		 * we're starting at the right offset in the chunk array.
		 */
		if (start_chunk >= lsm_tree->nchunks ||
		    lsm_tree->chunk[start_chunk]->id != start_id)
			for (start_chunk = 0;
			    start_chunk < lsm_tree->nchunks;
			    start_chunk++) {
				chunk = lsm_tree->chunk[start_chunk];
				if (chunk->id == start_id)
					break;
			}

		WT_ASSERT(session, start_chunk + nchunks <= lsm_tree->nchunks);
	} else {
		nchunks = lsm_tree->nchunks;

		/*
		 * If we are only opening the cursor for updates, only open the
		 * primary chunk, plus any other chunks that might be required
		 * to detect snapshot isolation conflicts.
		 */
		if (F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT))
			WT_ERR(__wt_realloc_def(session,
			    &clsm->txnid_alloc, nchunks,
			    &clsm->txnid_max));
		if (F_ISSET(clsm, WT_CLSM_OPEN_READ))
			ngood = 0;
		else if (F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT)) {
			/*
			 * Keep going until all updates in the next
			 * chunk are globally visible.  Copy the maximum
			 * transaction IDs into the cursor as we go.
			 */
			for (ngood = nchunks - 1; ngood > 0; ngood--) {
				chunk = lsm_tree->chunk[ngood - 1];
				clsm->txnid_max[ngood - 1] =
				    chunk->txnid_max;
				if (__wt_txn_visible_all(
				    session, chunk->txnid_max))
					break;
			}
		} else
			ngood = nchunks - 1;

		/* Check how many cursors are already open. */
		for (cp = clsm->cursors + ngood;
		    ngood < clsm->nchunks && ngood < nchunks;
		    cp++, ngood++) {
			chunk = lsm_tree->chunk[ngood];

			/* If the cursor isn't open yet, we're done. */
			if (*cp == NULL)
				break;

			/* Easy case: the URIs don't match. */
			if (strcmp((*cp)->uri, chunk->uri) != 0)
				break;

			/* Make sure the checkpoint config matches. */
			checkpoint = ((WT_CURSOR_BTREE *)*cp)->
			    btree->dhandle->checkpoint;
			if (checkpoint == NULL &&
			    F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) &&
			    !F_ISSET(chunk, WT_LSM_CHUNK_EMPTY))
				break;

			/* Make sure the Bloom config matches. */
			if (clsm->blooms[ngood] == NULL &&
			    F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
				break;
		}

		/* Spurious generation bump? */
		if (ngood == clsm->nchunks && clsm->nchunks == nchunks) {
			clsm->dsk_gen = lsm_tree->dsk_gen;
			goto err;
		}

		/*
		 * Close any cursors we no longer need.
		 *
		 * Drop the LSM tree lock while we do this: if the cache is
		 * full, we may block while closing a cursor.  Save the
		 * generation number and retry if it has changed under us.
		 */
		if (clsm->cursors != NULL && ngood < clsm->nchunks) {
			locked = 0;
			saved_gen = lsm_tree->dsk_gen;
			WT_ERR(__wt_rwunlock(session, lsm_tree->rwlock));
			WT_ERR(__clsm_close_cursors(clsm, ngood));
			WT_ERR(__wt_readlock(session, lsm_tree->rwlock));
			locked = 1;
			if (lsm_tree->dsk_gen != saved_gen)
				goto retry;
		}

		/* Detach from our old primary. */
		clsm->primary_chunk = NULL;
		clsm->current = NULL;
	}

	WT_ERR(__wt_realloc_def(session,
	    &clsm->bloom_alloc, nchunks, &clsm->blooms));
	WT_ERR(__wt_realloc_def(session,
	    &clsm->cursor_alloc, nchunks, &clsm->cursors));

	clsm->nchunks = nchunks;

	/* Open the cursors for chunks that have changed. */
	for (i = ngood, cp = clsm->cursors + i; i != nchunks; i++, cp++) {
		chunk = lsm_tree->chunk[i + start_chunk];
		/* Copy the maximum transaction ID. */
		if (F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT))
			clsm->txnid_max[i] = chunk->txnid_max;

		/*
		 * Read from the checkpoint if the file has been written.
		 * Once all cursors switch, the in-memory tree can be evicted.
		 */
		ret = __wt_open_cursor(session, chunk->uri, c,
		    (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) &&
		    !F_ISSET(chunk, WT_LSM_CHUNK_EMPTY)) ? ckpt_cfg : NULL, cp);

		/*
		 * XXX kludge: we may have an empty chunk where no checkpoint
		 * was written.  If so, try to open the ordinary handle on that
		 * chunk instead.
		 */
		if (ret == WT_NOTFOUND && F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)) {
			ret = __wt_open_cursor(session, chunk->uri, c,
			    NULL, cp);
			if (ret == 0)
				F_SET(chunk, WT_LSM_CHUNK_EMPTY);
		}
		WT_ERR(ret);

		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM) &&
		    !F_ISSET(clsm, WT_CLSM_MERGE))
			WT_ERR(__wt_bloom_open(session, chunk->bloom_uri,
			    lsm_tree->bloom_bit_count,
			    lsm_tree->bloom_hash_count,
			    c, &clsm->blooms[i]));

		/* Child cursors always use overwrite and raw mode. */
		F_SET(*cp, WT_CURSTD_OVERWRITE | WT_CURSTD_RAW);
	}

	/* The last chunk is our new primary. */
	if (chunk != NULL && !F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)) {
		clsm->primary_chunk = chunk;
		primary = clsm->cursors[clsm->nchunks - 1];
		WT_WITH_BTREE(session, ((WT_CURSOR_BTREE *)(primary))->btree,
		    __wt_btree_evictable(session, 0));
	}

	clsm->dsk_gen = lsm_tree->dsk_gen;
err:	F_CLR(session, WT_SESSION_NO_CACHE_CHECK);
#ifdef HAVE_DIAGNOSTIC
	/* Check that all cursors are open as expected. */
	if (ret == 0 && F_ISSET(clsm, WT_CLSM_OPEN_READ)) {
		for (i = 0, cp = clsm->cursors; i != nchunks; cp++, i++) {
			chunk = lsm_tree->chunk[i + start_chunk];

			/* Make sure the cursor is open. */
			WT_ASSERT(session, *cp != NULL);

			/* Easy case: the URIs should match. */
			WT_ASSERT(session, strcmp((*cp)->uri, chunk->uri) == 0);

			/* Make sure the checkpoint config matches. */
			checkpoint = ((WT_CURSOR_BTREE *)*cp)->
			    btree->dhandle->checkpoint;
			WT_ASSERT(session,
			    (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) &&
			    !F_ISSET(chunk, WT_LSM_CHUNK_EMPTY)) ?
			    checkpoint != NULL : checkpoint == NULL);

			/* Make sure the Bloom config matches. */
			WT_ASSERT(session,
			    (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM) &&
			    !F_ISSET(clsm, WT_CLSM_MERGE)) ?
			    clsm->blooms[i] != NULL : clsm->blooms[i] == NULL);
		}
	}
#endif
	if (locked)
		WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));
	return (ret);
}

/* __wt_clsm_init_merge --
 *	Initialize an LSM cursor for a merge.
 */
int
__wt_clsm_init_merge(
    WT_CURSOR *cursor, u_int start_chunk, uint32_t start_id, u_int nchunks)
{
	WT_CURSOR_LSM *clsm;

	clsm = (WT_CURSOR_LSM *)cursor;
	F_SET(clsm, WT_CLSM_MERGE);
	if (start_chunk != 0)
		F_SET(clsm, WT_CLSM_MINOR_MERGE);
	clsm->nchunks = nchunks;

	return (__clsm_open_cursors(clsm, 0, start_chunk, start_id));
}

/*
 * __clsm_get_current --
 *	Find the smallest / largest of the cursors and copy its key/value.
 */
static int
__clsm_get_current(
    WT_SESSION_IMPL *session, WT_CURSOR_LSM *clsm, int smallest, int *deletedp)
{
	WT_CURSOR *c, *current;
	int cmp, multiple;
	u_int i;

	current = NULL;
	multiple = 0;

	WT_FORALL_CURSORS(clsm, c, i) {
		if (!F_ISSET(c, WT_CURSTD_KEY_SET))
			continue;
		if (current == NULL) {
			current = c;
			continue;
		}
		WT_RET(WT_LSM_CURCMP(session, clsm->lsm_tree, c, current, cmp));
		if (smallest ? cmp < 0 : cmp > 0) {
			current = c;
			multiple = 0;
		} else if (cmp == 0)
			multiple = 1;
	}

	c = &clsm->iface;
	if ((clsm->current = current) == NULL) {
		F_CLR(c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
		return (WT_NOTFOUND);
	}

	if (multiple)
		F_SET(clsm, WT_CLSM_MULTIPLE);
	else
		F_CLR(clsm, WT_CLSM_MULTIPLE);

	WT_RET(current->get_key(current, &c->key));
	WT_RET(current->get_value(current, &c->value));

	if ((*deletedp = __clsm_deleted(clsm, &c->value)) == 0) {
		F_CLR(c, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
		F_SET(c, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
	} else
		F_CLR(c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	return (0);
}

/*
 * __clsm_compare --
 *	WT_CURSOR->compare implementation for the LSM cursor type.
 */
static int
__clsm_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
	WT_CURSOR_LSM *alsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	int cmp;

	/* There's no need to sync with the LSM tree, avoid WT_LSM_ENTER. */
	alsm = (WT_CURSOR_LSM *)a;
	CURSOR_API_CALL(a, session, compare, NULL);

	/*
	 * Confirm both cursors refer to the same source and have keys, then
	 * compare the keys.
	 */
	if (strcmp(a->uri, b->uri) != 0)
		WT_ERR_MSG(session, EINVAL,
		    "comparison method cursors must reference the same object");

	WT_CURSOR_NEEDKEY(a);
	WT_CURSOR_NEEDKEY(b);

	WT_ERR(WT_LEX_CMP(
	    session, alsm->lsm_tree->collator, &a->key, &b->key, cmp));
	*cmpp = cmp;

err:	API_END(session);
	return (ret);
}

/*
 * __clsm_next --
 *	WT_CURSOR->next method for the LSM cursor type.
 */
static int
__clsm_next(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int i;
	int check, cmp, deleted;

	WT_LSM_ENTER(clsm, cursor, session, next);

	/* If we aren't positioned for a forward scan, get started. */
	if (clsm->current == NULL || !F_ISSET(clsm, WT_CLSM_ITERATE_NEXT)) {
		F_CLR(clsm, WT_CLSM_MULTIPLE);
		WT_FORALL_CURSORS(clsm, c, i) {
			if (!F_ISSET(cursor, WT_CURSTD_KEY_SET)) {
				WT_ERR(c->reset(c));
				ret = c->next(c);
			} else if (c != clsm->current) {
				c->set_key(c, &cursor->key);
				if ((ret = c->search_near(c, &cmp)) == 0) {
					if (cmp < 0)
						ret = c->next(c);
					else if (cmp == 0) {
						if (clsm->current == NULL)
							clsm->current = c;
						else
							F_SET(clsm,
							    WT_CLSM_MULTIPLE);
					}
				} else
					F_CLR(c, WT_CURSTD_KEY_SET);
			}
			WT_ERR_NOTFOUND_OK(ret);
		}
		F_SET(clsm, WT_CLSM_ITERATE_NEXT);
		F_CLR(clsm, WT_CLSM_ITERATE_PREV);

		/* We just positioned *at* the key, now move. */
		if (clsm->current != NULL)
			goto retry;
	} else {
retry:		/*
		 * If there are multiple cursors on that key, move them
		 * forward.
		 */
		if (F_ISSET(clsm, WT_CLSM_MULTIPLE)) {
			check = 0;
			WT_FORALL_CURSORS(clsm, c, i) {
				if (!F_ISSET(c, WT_CURSTD_KEY_SET))
					continue;
				if (check) {
					WT_ERR(WT_LSM_CURCMP(session,
					    clsm->lsm_tree, c, clsm->current,
					    cmp));
					if (cmp == 0)
						WT_ERR_NOTFOUND_OK(c->next(c));
				}
				if (c == clsm->current)
					check = 1;
			}
		}

		/* Move the smallest cursor forward. */
		c = clsm->current;
		WT_ERR_NOTFOUND_OK(c->next(c));
	}

	/* Find the cursor(s) with the smallest key. */
	if ((ret = __clsm_get_current(session, clsm, 1, &deleted)) == 0 &&
	    deleted)
		goto retry;

err:	API_END(session);
	return (ret);
}

/*
 * __clsm_prev --
 *	WT_CURSOR->prev method for the LSM cursor type.
 */
static int
__clsm_prev(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int i;
	int check, cmp, deleted;

	WT_LSM_ENTER(clsm, cursor, session, prev);

	/* If we aren't positioned for a reverse scan, get started. */
	if (clsm->current == NULL || !F_ISSET(clsm, WT_CLSM_ITERATE_PREV)) {
		F_CLR(clsm, WT_CLSM_MULTIPLE);
		WT_FORALL_CURSORS(clsm, c, i) {
			if (!F_ISSET(cursor, WT_CURSTD_KEY_SET)) {
				WT_ERR(c->reset(c));
				ret = c->prev(c);
			} else if (c != clsm->current) {
				c->set_key(c, &cursor->key);
				if ((ret = c->search_near(c, &cmp)) == 0) {
					if (cmp > 0)
						ret = c->prev(c);
					else if (cmp == 0) {
						if (clsm->current == NULL)
							clsm->current = c;
						else
							F_SET(clsm,
							    WT_CLSM_MULTIPLE);
					}
				}
			}
			WT_ERR_NOTFOUND_OK(ret);
		}
		F_SET(clsm, WT_CLSM_ITERATE_PREV);
		F_CLR(clsm, WT_CLSM_ITERATE_NEXT);

		/* We just positioned *at* the key, now move. */
		if (clsm->current != NULL)
			goto retry;
	} else {
retry:		/*
		 * If there are multiple cursors on that key, move them
		 * backwards.
		 */
		if (F_ISSET(clsm, WT_CLSM_MULTIPLE)) {
			check = 0;
			WT_FORALL_CURSORS(clsm, c, i) {
				if (!F_ISSET(c, WT_CURSTD_KEY_SET))
					continue;
				if (check) {
					WT_ERR(WT_LSM_CURCMP(session,
					    clsm->lsm_tree, c, clsm->current,
					    cmp));
					if (cmp == 0)
						WT_ERR_NOTFOUND_OK(c->prev(c));
				}
				if (c == clsm->current)
					check = 1;
			}
		}

		/* Move the smallest cursor backwards. */
		c = clsm->current;
		WT_ERR_NOTFOUND_OK(c->prev(c));
	}

	/* Find the cursor(s) with the largest key. */
	if ((ret = __clsm_get_current(session, clsm, 0, &deleted)) == 0 &&
	    deleted)
		goto retry;

err:	API_END(session);
	return (ret);
}

/*
 * __clsm_reset_cursors --
 *	Reset any positioned chunk cursors.
 */
static int
__clsm_reset_cursors(WT_CURSOR_LSM *clsm)
{
	WT_CURSOR *c;
	WT_DECL_RET;
	u_int i;

	WT_FORALL_CURSORS(clsm, c, i)
		if (F_ISSET(c, WT_CURSTD_KEY_SET))
			WT_TRET(c->reset(c));

	clsm->current = NULL;
	F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);

	return (ret);
}

/*
 * __clsm_reset --
 *	WT_CURSOR->reset method for the LSM cursor type.
 */
static int
__clsm_reset(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	/*
	 * Don't use the normal __clsm_enter path: that is wasted work when all
	 * we want to do is give up our position.
	 */
	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL(cursor, session, reset, NULL);
	ret = __clsm_reset_cursors(clsm);
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

err:	API_END(session);
	return (ret);
}

/*
 * __clsm_search --
 *	WT_CURSOR->search method for the LSM cursor type.
 */
static int
__clsm_search(WT_CURSOR *cursor)
{
	WT_BLOOM *bloom;
	WT_BLOOM_HASH bhash;
	WT_CURSOR *c;
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int i;
	int have_hash;

	have_hash = 0;

	WT_LSM_ENTER(clsm, cursor, session, search);
	WT_CURSOR_NEEDKEY(cursor);
	F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);

	/* Reset any positioned cursor(s) to release pinned resources. */
	WT_ERR(__clsm_reset_cursors(clsm));

	WT_FORALL_CURSORS(clsm, c, i) {
		/* If there is a Bloom filter, see if we can skip the read. */
		if ((bloom = clsm->blooms[i]) != NULL) {
			if (!have_hash) {
				WT_ERR(__wt_bloom_hash(
				    bloom, &cursor->key, &bhash));
				have_hash = 1;
			}

			ret = __wt_bloom_hash_get(bloom, &bhash);
			if (ret == WT_NOTFOUND) {
				WT_STAT_INCR(session,
				    &clsm->lsm_tree->stats, bloom_miss);
				continue;
			} else if (ret == 0)
				WT_STAT_INCR(session,
				    &clsm->lsm_tree->stats, bloom_hit);
			WT_ERR(ret);
		}
		c->set_key(c, &cursor->key);
		if ((ret = c->search(c)) == 0) {
			WT_ERR(c->get_key(c, &cursor->key));
			WT_ERR(c->get_value(c, &cursor->value));
			clsm->current = c;
			if (__clsm_deleted(clsm, &cursor->value))
				ret = WT_NOTFOUND;
			goto done;
		}
		WT_ERR_NOTFOUND_OK(ret);
		F_CLR(c, WT_CURSTD_KEY_SET);
		/* Update stats: the active chunk can't have a bloom filter. */
		if (bloom != NULL)
			WT_STAT_INCR(session,
			    &clsm->lsm_tree->stats, bloom_false_positive);
		else if (clsm->primary_chunk == NULL || i != clsm->nchunks)
			WT_STAT_INCR(session,
			    &clsm->lsm_tree->stats, lsm_lookup_no_bloom);
	}
	ret = WT_NOTFOUND;

done:
err:	API_END(session);
	if (ret == 0) {
		F_CLR(cursor, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
		F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
	} else {
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
		clsm->current = NULL;
	}

	return (ret);
}

/*
 * __clsm_search_near --
 *	WT_CURSOR->search_near method for the LSM cursor type.
 */
static int
__clsm_search_near(WT_CURSOR *cursor, int *exactp)
{
	WT_CURSOR *c, *larger, *smaller;
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_ITEM v;
	WT_SESSION_IMPL *session;
	u_int i;
	int cmp, deleted;

	larger = smaller = NULL;

	WT_LSM_ENTER(clsm, cursor, session, search_near);
	WT_CURSOR_NEEDKEY(cursor);
	F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);

	/* Reset any positioned cursor(s) to release pinned resources. */
	WT_ERR(__clsm_reset_cursors(clsm));

	/*
	 * search_near is somewhat fiddly: we can't just return a nearby key
	 * from the in-memory chunk because there could be a closer key on
	 * disk.
	 *
	 * As we search down the chunks, we stop as soon as we find an exact
	 * match.  Otherwise, we maintain the smallest cursor larger than the
	 * search key and the largest cursor smaller than the search key.  At
	 * the bottom, we prefer the larger cursor, but if no record is larger,
	 * return the smaller cursor, or if no record at all was found,
	 * WT_NOTFOUND.
	 */
	WT_FORALL_CURSORS(clsm, c, i) {
		c->set_key(c, &cursor->key);
		if ((ret = c->search_near(c, &cmp)) == WT_NOTFOUND) {
			F_CLR(c, WT_CURSTD_KEY_SET);
			ret = 0;
			continue;
		} else if (ret != 0)
			goto err;

		WT_ERR(c->get_value(c, &v));
		deleted = __clsm_deleted(clsm, &v);

		if (cmp == 0 && !deleted) {
			clsm->current = c;
			*exactp = 0;
			goto done;
		}

		/*
		 * Prefer larger cursors.  There are two reasons: (1) we expect
		 * prefix searches to be a common case (as in our own indices);
		 * and (2) we need a way to unambiguously know we have the
		 * "closest" result.
		 */
		if (cmp < 0) {
			if ((ret = c->next(c)) == 0)
				cmp = 1;
			else if (ret == WT_NOTFOUND)
				ret = c->prev(c);
			if (ret != 0)
				goto err;
		}

		/*
		 * If we land on a deleted item, try going forwards or
		 * backwards to find one that isn't deleted.
		 */
		while (deleted && (ret = c->next(c)) == 0) {
			cmp = 1;
			WT_ERR(c->get_value(c, &v));
			deleted = __clsm_deleted(clsm, &v);
		}
		WT_ERR_NOTFOUND_OK(ret);
		while (deleted && (ret = c->prev(c)) == 0) {
			cmp = -1;
			WT_ERR(c->get_value(c, &v));
			deleted = __clsm_deleted(clsm, &v);
		}
		WT_ERR_NOTFOUND_OK(ret);
		if (deleted)
			continue;

		/*
		 * We are trying to find the smallest cursor greater than the
		 * search key, or, if there is no larger key, the largest
		 * cursor smaller than the search key.
		 *
		 * It could happen that one cursor contains both of the closest
		 * records.  In that case, we will track it in "larger", and it
		 * will be the one we finally choose.
		 */
		if (cmp > 0) {
			if (larger == NULL)
				larger = c;
			else {
				WT_ERR(WT_LSM_CURCMP(session,
				    clsm->lsm_tree, c, larger, cmp));
				if (cmp < 0) {
					WT_ERR(larger->reset(larger));
					larger = c;
				}
			}
		} else {
			if (smaller == NULL)
				smaller = c;
			else {
				WT_ERR(WT_LSM_CURCMP(session,
				    clsm->lsm_tree, c, smaller, cmp));
				if (cmp > 0) {
					WT_ERR(smaller->reset(smaller));
					smaller = c;
				}
			}
		}

		if (c != smaller && c != larger)
			WT_ERR(c->reset(c));
	}

	if (larger != NULL) {
		clsm->current = larger;
		larger = NULL;
		*exactp = 1;
	} else if (smaller != NULL) {
		clsm->current = smaller;
		smaller = NULL;
		*exactp = -1;
	} else
		ret = WT_NOTFOUND;

done:
err:	API_END(session);
	if (ret == 0) {
		c = clsm->current;
		WT_TRET(c->get_key(c, &cursor->key));
		WT_TRET(c->get_value(c, &cursor->value));
	}
	if (smaller != NULL)
		WT_TRET(smaller->reset(smaller));
	if (larger != NULL)
		WT_TRET(larger->reset(larger));

	if (ret == 0) {
		F_CLR(cursor, WT_CURSTD_KEY_EXT | WT_CURSTD_VALUE_EXT);
		F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
	} else {
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
		clsm->current = NULL;
	}

	return (ret);
}

/*
 * __clsm_put --
 *	Put an entry into the in-memory tree, trigger a file switch if
 *	necessary.
 */
static inline int
__clsm_put(WT_SESSION_IMPL *session,
    WT_CURSOR_LSM *clsm, const WT_ITEM *key, const WT_ITEM *value, int position)
{
	WT_CURSOR *c, *primary;
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;
	u_int i;
	int need_signal, ovfl;

	lsm_tree = clsm->lsm_tree;

	WT_ASSERT(session, clsm->primary_chunk != NULL);
	WT_ASSERT(session, !F_ISSET(clsm->primary_chunk, WT_LSM_CHUNK_ONDISK));
	WT_ASSERT(session,
	    TXNID_LE(session->txn.id, clsm->primary_chunk->txnid_max));

	/* If necessary, set the position for future scans. */
	WT_RET(__clsm_reset_cursors(clsm));
	primary = clsm->cursors[clsm->nchunks - 1];
	if (position)
		clsm->current = primary;

	/*
	 * Update the primary, plus any older chunks needed to detect
	 * write-write conflicts across chunk boundaries.
	 */
	for (i = 0; i < clsm->nupdates; i++) {
		c = clsm->cursors[(clsm->nchunks - i) - 1];
		c->set_key(c, key);
		c->set_value(c, value);
		WT_RET((position && i == 0) ? c->update(c) : c->insert(c));
	}

	/*
	 * The count is in a shared structure, but it's only approximate, so
	 * don't worry about protecting access.
	 */
	if (++clsm->primary_chunk->count % 100 == 0 &&
	    lsm_tree->throttle_sleep > 0)
		__wt_sleep(0, lsm_tree->throttle_sleep);

	/*
	 * In LSM there are multiple btrees active at one time. The tree
	 * switch code needs to use btree API methods, and it wants to
	 * operate on the btree for the primary chunk. Set that up now.
	 *
	 * If the primary chunk has grown too large, set a flag so the worker
	 * thread will switch when it gets a chance to avoid introducing high
	 * latency into application threads.  Don't do this indefinitely: if a
	 * chunk grows twice as large as the configured size, block until it
	 * can be switched.
	 */
	if (!F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH)) {
		WT_WITH_BTREE(session, ((WT_CURSOR_BTREE *)primary)->btree,
		    ovfl = __wt_btree_size_overflow(
		    session, lsm_tree->chunk_size));

		if (ovfl) {
			/*
			 * Check that we are up-to-date: don't set the switch
			 * if the tree has changed since we last opened
			 * cursors: that can lead to switching multiple times
			 * when only one switch is required, creating very
			 * small chunks.
			 */
			need_signal = 0;
			WT_RET(__wt_readlock(session, lsm_tree->rwlock));
			if (clsm->dsk_gen == lsm_tree->dsk_gen &&
			    !F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH)) {
				F_SET(lsm_tree, WT_LSM_TREE_NEED_SWITCH);
				need_signal = 1;
			}
			WT_RET(__wt_rwunlock(session, lsm_tree->rwlock));
			if (need_signal)
				WT_RET(__wt_cond_signal(
				    session, lsm_tree->work_cond));
			ovfl = 0;
		}
	} else
		WT_WITH_BTREE(session, ((WT_CURSOR_BTREE *)primary)->btree,
		    ovfl = __wt_btree_size_overflow(
		    session, 2 * lsm_tree->chunk_size));

	if (ovfl) {
		WT_RET(__wt_writelock(session, lsm_tree->rwlock));
		if (clsm->dsk_gen == lsm_tree->dsk_gen)
			WT_WITH_SCHEMA_LOCK(session,
			    ret = __wt_lsm_tree_switch(session, lsm_tree));
		WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));
		WT_RET(ret);
	}

	return (0);
}

/*
 * __clsm_insert --
 *	WT_CURSOR->insert method for the LSM cursor type.
 */
static int
__clsm_insert(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	WT_LSM_UPDATE_ENTER(clsm, cursor, session, insert);
	WT_CURSOR_NEEDKEY(cursor);
	WT_LSM_NEEDVALUE(cursor);

	if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE) &&
	    (ret = __clsm_search(cursor)) != WT_NOTFOUND) {
		if (ret == 0)
			ret = WT_DUPLICATE_KEY;
		return (ret);
	}

	ret = __clsm_put(session, clsm, &cursor->key, &cursor->value, 0);

err:	WT_LSM_UPDATE_LEAVE(clsm, session, ret);
	return (ret);
}

/*
 * __clsm_update --
 *	WT_CURSOR->update method for the LSM cursor type.
 */
static int
__clsm_update(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	WT_LSM_UPDATE_ENTER(clsm, cursor, session, update);
	WT_CURSOR_NEEDKEY(cursor);
	WT_LSM_NEEDVALUE(cursor);

	if (F_ISSET(cursor, WT_CURSTD_OVERWRITE) ||
	    (ret = __clsm_search(cursor)) == 0)
		ret = __clsm_put(
		    session, clsm, &cursor->key, &cursor->value, 1);

err:	WT_LSM_UPDATE_LEAVE(clsm, session, ret);
	return (ret);
}

/*
 * __clsm_remove --
 *	WT_CURSOR->remove method for the LSM cursor type.
 */
static int
__clsm_remove(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	WT_LSM_UPDATE_ENTER(clsm, cursor, session, remove);
	WT_CURSOR_NEEDKEY(cursor);

	if (F_ISSET(cursor, WT_CURSTD_OVERWRITE) ||
	    (ret = __clsm_search(cursor)) == 0)
		ret = __clsm_put(
		    session, clsm, &cursor->key, &__lsm_tombstone, 1);

err:	WT_LSM_UPDATE_LEAVE(clsm, session, ret);
	return (ret);
}

/*
 * __clsm_close --
 *	WT_CURSOR->close method for the LSM cursor type.
 */
static int
__clsm_close(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	/*
	 * Don't use the normal __clsm_enter path: that is wasted work when
	 * closing, and the cursor may never have been used.
	 */
	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL(cursor, session, close, NULL);
	WT_TRET(__clsm_close_cursors(clsm, 0));
	__wt_free(session, clsm->blooms);
	__wt_free(session, clsm->cursors);
	__wt_free(session, clsm->txnid_max);

	/* The WT_LSM_TREE owns the URI. */
	cursor->uri = NULL;
	if (clsm->lsm_tree != NULL)
		__wt_lsm_tree_release(session, clsm->lsm_tree);
	WT_TRET(__wt_cursor_close(cursor));

err:	API_END(session);
	return (ret);
}

/*
 * __wt_clsm_open --
 *	WT_SESSION->open_cursor method for LSM cursors.
 */
int
__wt_clsm_open(WT_SESSION_IMPL *session,
    const char *uri, WT_CURSOR *owner, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_CURSOR_STATIC_INIT(iface,
	    NULL,			/* get-key */
	    NULL,			/* get-value */
	    NULL,			/* set-key */
	    NULL,			/* set-value */
	    __clsm_compare,		/* compare */
	    __clsm_next,		/* next */
	    __clsm_prev,		/* prev */
	    __clsm_reset,		/* reset */
	    __clsm_search,		/* search */
	    __clsm_search_near,		/* search-near */
	    __clsm_insert,		/* insert */
	    __clsm_update,		/* update */
	    __clsm_remove,		/* remove */
	    __clsm_close);		/* close */
	WT_CURSOR *cursor;
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;

	clsm = NULL;
	cursor = NULL;

	if (!WT_PREFIX_MATCH(uri, "lsm:"))
		return (EINVAL);

	/* Get the LSM tree. */
	WT_WITH_SCHEMA_LOCK_OPT(session,
	    ret = __wt_lsm_tree_get(session, uri, 0, &lsm_tree));
	WT_RET(ret);

	WT_ERR(__wt_calloc_def(session, 1, &clsm));

	cursor = &clsm->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->uri = lsm_tree->name;
	cursor->key_format = lsm_tree->key_format;
	cursor->value_format = lsm_tree->value_format;

	clsm->lsm_tree = lsm_tree;

	/*
	 * The tree's dsk_gen starts at one, so starting the cursor on zero
	 * will force a call into open_cursors on the first operation.
	 */
	clsm->dsk_gen = 0;

	STATIC_ASSERT(offsetof(WT_CURSOR_LSM, iface) == 0);
	WT_ERR(__wt_cursor_init(cursor, cursor->uri, owner, cfg, cursorp));

	if (0) {
err:		if (lsm_tree != NULL)
			__wt_lsm_tree_release(session, lsm_tree);
		if (cursor != NULL)
			WT_TRET(__clsm_close(cursor));
	}

	return (ret);
}
