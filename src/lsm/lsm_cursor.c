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

#define	WT_LSM_CMP(s, lsm_tree, k1, k2, cmp)				\
	(((lsm_tree)->collator == NULL) ?				\
	(((cmp) = __wt_btree_lex_compare((k1), (k2))), 0) :		\
	(lsm_tree)->collator->compare((lsm_tree)->collator,		\
	    &(s)->iface, (k1), (k2), &(cmp)))

#define	WT_LSM_CURCMP(s, lsm_tree, c1, c2, cmp)				\
	WT_LSM_CMP(s, lsm_tree, &(c1)->key, &(c2)->key, cmp)

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

static int __clsm_open_cursors(WT_CURSOR_LSM *, int, u_int, uint32_t);
static int __clsm_search(WT_CURSOR *);

static inline int
__clsm_enter(WT_CURSOR_LSM *clsm, int update)
{
	if (!F_ISSET(clsm, WT_CLSM_MERGE) &&
	    (clsm->dsk_gen != clsm->lsm_tree->dsk_gen ||
	    (!update && !F_ISSET(clsm, WT_CLSM_OPEN_READ))))
		WT_RET(__clsm_open_cursors(clsm, update, 0, 0));

	return (0);
}

/*
 * TODO: use something other than an empty value as a tombstone: we need
 * to support empty values from the application.
 */
static WT_ITEM __lsm_tombstone = { "", 0, 0, NULL, 0 };

#define	WT_LSM_NEEDVALUE(c) do {					\
	WT_CURSOR_NEEDVALUE(c);						\
	if (__clsm_deleted((WT_CURSOR_LSM *)(c), &(c)->value))		\
		WT_ERR(__wt_cursor_kv_not_set(cursor, 0));		\
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
 *	Close all of the btree cursors currently open.
 */
static int
__clsm_close_cursors(WT_CURSOR_LSM *clsm)
{
	WT_BLOOM *bloom;
	WT_CURSOR *c;
	u_int i;

	if (clsm->cursors == NULL)
		return (0);

	/* Detach from our old primary. */
	if (clsm->primary_chunk != NULL) {
		(void)WT_ATOMIC_SUB(clsm->primary_chunk->ncursor, 1);
		clsm->primary_chunk = NULL;
	}

	WT_FORALL_CURSORS(clsm, c, i) {
		clsm->cursors[i] = NULL;
		WT_RET(c->close(c));
		if ((bloom = clsm->blooms[i]) != NULL) {
			clsm->blooms[i] = NULL;
			WT_RET(__wt_bloom_close(bloom));
		}
	}

	clsm->current = NULL;
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
	WT_CURSOR *c, **cp;
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;
	const char *ckpt_cfg[] = API_CONF_DEFAULTS(session, open_cursor,
	    "checkpoint=WiredTigerCheckpoint,raw");
	const char *merge_cfg[] = API_CONF_DEFAULTS(session, open_cursor,
	    "checkpoint=WiredTigerCheckpoint,raw");
	u_int i, nchunks;

	session = (WT_SESSION_IMPL *)clsm->iface.session;
	lsm_tree = clsm->lsm_tree;
	c = &clsm->iface;
	chunk = NULL;

	if (!update)
		F_SET(clsm, WT_CLSM_OPEN_READ);

	/* Copy the key, so we don't lose the cursor position. */
	if (F_ISSET(c, WT_CURSTD_KEY_RET)) {
		F_CLR(c, WT_CURSTD_KEY_RET);
		if (c->key.data != c->key.mem)
			WT_RET(__wt_buf_set(
			    session, &c->key, c->key.data, c->key.size));
		F_SET(c, WT_CURSTD_KEY_APP);
	}
	F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);

	WT_RET(__clsm_close_cursors(clsm));

	WT_RET(__wt_readlock(session, lsm_tree->rwlock));
	F_SET(session, WT_SESSION_NO_CACHE_CHECK);

	/* Merge cursors have already figured out how many chunks they need. */
	if (F_ISSET(clsm, WT_CLSM_MERGE)) {
		nchunks = clsm->nchunks;

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
	} else
		nchunks = lsm_tree->nchunks;

	if (clsm->cursors == NULL || nchunks > clsm->nchunks) {
		WT_ERR(__wt_realloc(session, NULL,
		    nchunks * sizeof(WT_BLOOM *), &clsm->blooms));
		WT_ERR(__wt_realloc(session, NULL,
		    nchunks * sizeof(WT_CURSOR *), &clsm->cursors));
	}
	clsm->nchunks = nchunks;

	for (i = 0, cp = clsm->cursors; i != clsm->nchunks; i++, cp++) {
		if (!F_ISSET(clsm, WT_CLSM_OPEN_READ) && i < clsm->nchunks - 1)
			continue;

		/*
		 * Read from the checkpoint if the file has been written.
		 * Once all cursors switch, the in-memory tree can be evicted.
		 */
		chunk = lsm_tree->chunk[i + start_chunk];
		ret = __wt_open_cursor(session,
		    chunk->uri, &clsm->iface,
		    !F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) ? NULL :
		    (F_ISSET(clsm, WT_CLSM_MERGE) ? merge_cfg : ckpt_cfg), cp);

		/*
		 * XXX kludge: we may have an empty chunk where no checkpoint
		 * was written.  If so, try to open the ordinary handle on that
		 * chunk instead.
		 */
		if (ret == WT_NOTFOUND && F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
			ret = __wt_open_cursor(session,
			    chunk->uri, &clsm->iface, NULL, cp);
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
		(void)WT_ATOMIC_ADD(clsm->primary_chunk->ncursor, 1);

		/*
		 * Peek into the btree layer to track the in-memory size.
		 * Ignore error returns since it is OK for the btree to be
		 * empty in this code path (and that is an error condition).
		 */
		if (lsm_tree->memsizep == NULL)
			(void)__wt_btree_get_memsize(
			    session, S2BT(session), &lsm_tree->memsizep);
	}

	clsm->dsk_gen = lsm_tree->dsk_gen;
err:	F_CLR(session, WT_SESSION_NO_CACHE_CHECK);
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
		F_CLR(c, WT_CURSTD_KEY_APP | WT_CURSTD_VALUE_APP);
		F_SET(c, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);
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

	WT_ERR(WT_LSM_CMP(session, alsm->lsm_tree, &a->key, &b->key, cmp));
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
				}
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
 * __clsm_reset --
 *	WT_CURSOR->reset method for the LSM cursor type.
 */
static int
__clsm_reset(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	/*
	 * Don't use the normal __clsm_enter path: that is wasted work when all
	 * we want to do is give up our position.
	 */
	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL(cursor, session, reset, NULL);
	if ((c = clsm->current) != NULL) {
		ret = c->reset(c);
		clsm->current = NULL;
	}
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);

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

	/*
	 * Reset any positioned cursor(s) to release pinned resources.
	 *
	 * TODO: generalize this and apply to all methods that don't preserve
	 * position.
	 */
	if (clsm->current != NULL) {
		WT_ERR(clsm->current->reset(clsm->current));
		clsm->current = NULL;
	}

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
				WT_STAT_INCR(
				    clsm->lsm_tree->stats, bloom_miss);
				continue;
			} else if (ret == 0)
				WT_STAT_INCR(clsm->lsm_tree->stats, bloom_hit);
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
		} else if (ret != WT_NOTFOUND)
			goto err;
		else if (bloom != NULL)
			WT_STAT_INCR(
			    clsm->lsm_tree->stats, bloom_false_positive);
		/* The active chunk can't have a bloom filter. */
		else if (clsm->primary_chunk == NULL || i != clsm->nchunks)
			WT_STAT_INCR(
			    clsm->lsm_tree->stats, lsm_lookup_no_bloom);
	}
	ret = WT_NOTFOUND;

done:
err:	API_END(session);
	if (ret == 0) {
		F_CLR(cursor, WT_CURSTD_KEY_APP | WT_CURSTD_VALUE_APP);
		F_SET(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);
	} else
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

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

	/*
	 * Reset any positioned cursor(s) to release pinned resources.
	 *
	 * TODO: generalize this and apply to all methods that don't preserve
	 * position.
	 */
	if (clsm->current != NULL) {
		WT_ERR(clsm->current->reset(clsm->current));
		clsm->current = NULL;
	}

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
		F_CLR(cursor, WT_CURSTD_KEY_APP | WT_CURSTD_VALUE_APP);
		F_SET(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);
	} else
		F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	return (ret);
}

/*
 * __clsm_put --
 *	Put an entry into the in-memory tree, trigger a file switch if
 *	necessary.
 */
static inline int
__clsm_put(
    WT_SESSION_IMPL *session, WT_CURSOR_LSM *clsm, WT_ITEM *key, WT_ITEM *value)
{
	WT_BTREE *btree;
	WT_CURSOR *primary;
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;
	uint32_t *memsizep;

	lsm_tree = clsm->lsm_tree;

	/*
	 * If this is the first update in this cursor, check if a new in-memory
	 * chunk is needed.
	 */
	if (clsm->primary_chunk == NULL) {
		WT_RET(__wt_writelock(session, lsm_tree->rwlock));
		if (clsm->dsk_gen == lsm_tree->dsk_gen)
			WT_WITH_SCHEMA_LOCK(session,
			    ret = __wt_lsm_tree_switch(session, lsm_tree));
		WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));
		WT_RET(ret);

		/* We changed the structure, or someone else did: update. */
		WT_RET(__clsm_enter(clsm, 1));

		WT_ASSERT(session, clsm->primary_chunk != NULL);
	}

	primary = clsm->cursors[clsm->nchunks - 1];
	primary->set_key(primary, key);
	primary->set_value(primary, value);
	WT_RET(primary->insert(primary));

	/*
	 * The count is in a shared structure, but it's only approximate, so
	 * don't worry about protecting access.
	 */
	++clsm->primary_chunk->count;

	/*
	 * Set the position for future scans.  If we were already positioned in
	 * a non-primary chunk, we may now have multiple cursors matching the
	 * key.
	 */
	F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);
	clsm->current = primary;

	if ((memsizep = lsm_tree->memsizep) != NULL &&
	    *memsizep > lsm_tree->chunk_size) {
		/*
		 * Take the LSM lock first: we can't acquire it while
		 * holding the schema lock, or we will deadlock.
		 */
		WT_RET(__wt_writelock(session, lsm_tree->rwlock));
		/* Make sure we don't race. */
		if (clsm->dsk_gen == lsm_tree->dsk_gen)
			WT_WITH_SCHEMA_LOCK(session,
			    ret = __wt_lsm_tree_switch(session, lsm_tree));

		/*
		 * Clear the "cache resident" flag so the primary can be
		 * evicted and eventually closed.  Make sure we succeeded
		 * in switching: if something went wrong, we should keep
		 * trying to switch.
		 */
		btree = ((WT_CURSOR_BTREE *)primary)->btree;
		if (ret == 0)
			ret = __wt_btree_release_memsize(session, btree);

		WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));
	}

	return (ret);
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

	ret = __clsm_put(session, clsm, &cursor->key, &cursor->value);

err:	CURSOR_UPDATE_API_END(session, ret);
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
		ret = __clsm_put(session, clsm, &cursor->key, &cursor->value);

err:	CURSOR_UPDATE_API_END(session, ret);
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
		ret = __clsm_put(session, clsm, &cursor->key, &__lsm_tombstone);

err:	CURSOR_UPDATE_API_END(session, ret);
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
	WT_TRET(__clsm_close_cursors(clsm));
	__wt_free(session, clsm->blooms);
	__wt_free(session, clsm->cursors);
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
	WT_CONFIG_ITEM cval;
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

	/*
	 * LSM cursors default to overwrite: if no setting was supplied, turn
	 * it on.
	 */
	if (cfg == NULL || cfg[1] == NULL || __wt_config_getones(
	    session, cfg[1], "overwrite", &cval) == WT_NOTFOUND)
		F_SET(cursor, WT_CURSTD_OVERWRITE);

	if (0) {
err:		if (lsm_tree != NULL)
			__wt_lsm_tree_release(session, lsm_tree);
		if (cursor != NULL)
			WT_TRET(__clsm_close(cursor));
	}

	return (ret);
}
