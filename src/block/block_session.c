/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Per session handle cached block manager information.
 */
typedef struct {
#define	WT_BLOCK_MGR_CACHE_EXT_MAX	100
	WT_EXT  *ext_cache;			/* List of WT_EXT handles */
	u_int    ext_cache_cnt;			/* Count */

#define	WT_BLOCK_MGR_CACHE_SIZE_MAX	10
	WT_SIZE *sz_cache;			/* List of WT_SIZE handles */
	u_int    sz_cache_cnt;			/* Count */
} WT_BLOCK_MGR_SESSION;

static int __block_manager_session_cleanup(WT_SESSION_IMPL *);

/*
 * __block_ext_alloc --
 *	Allocate a new WT_EXT structure.
 */
static int
__block_ext_alloc(WT_SESSION_IMPL *session, WT_EXT **extp)
{
	WT_EXT *ext;

	u_int skipdepth;

	skipdepth = __wt_skip_choose_depth();
	WT_RET(__wt_calloc(session, 1,
	    sizeof(WT_EXT) + skipdepth * 2 * sizeof(WT_EXT *), &ext));
	ext->depth = (uint8_t)skipdepth;
	(*extp) = ext;

	return (0);
}

/*
 * __wt_block_ext_alloc --
 *	Allocate WT_EXT structures.
 */
int
__wt_block_ext_alloc(WT_SESSION_IMPL *session, WT_EXT **extp, u_int max)
{
	WT_BLOCK_MGR_SESSION *bms;
	WT_EXT *ext;

	bms = session->block_manager;

	/* First, return a WT_EXT structure for use from a cached list. */
	if (extp != NULL && bms != NULL && bms->ext_cache != NULL) {
		(*extp) = bms->ext_cache;
		bms->ext_cache = bms->ext_cache->next[0];

		/*
		 * The count is advisory to minimize our exposure to bugs, but
		 * don't let it go negative, that would lead to never caching
		 * a WT_EXT structure again, we'd always believe there are too
		 * many cached.
		 */
		if (bms->ext_cache_cnt > 0)
			--bms->ext_cache_cnt;
		return (0);
	}

	/* Second, allocating a WT_EXT structure for use. */
	if (extp != NULL) {
		WT_STAT_FAST_CONN_INCR(session, block_locked_allocation);
		return (__block_ext_alloc(session, extp));
	}

	/* Third, pre-allocating WT_EXT structures for later use. */
	if (bms == NULL) {
		WT_RET(__wt_calloc(session,
		    1, sizeof(WT_BLOCK_MGR_SESSION), &session->block_manager));
		bms = session->block_manager;
		session->block_manager_cleanup =
		    __block_manager_session_cleanup;
	}
	for (; bms->ext_cache_cnt < max; ++bms->ext_cache_cnt) {
		WT_RET(__block_ext_alloc(session, &ext));

		ext->next[0] = bms->ext_cache;
		bms->ext_cache = ext;
	}
	return (0);
}

/*
 * __wt_block_ext_free --
 *	Add an WT_EXT structure to the cached list, or free if enough cached.
 */
void
__wt_block_ext_free(WT_SESSION_IMPL *session, WT_EXT *ext)
{
	WT_BLOCK_MGR_SESSION *bms;

	bms = session->block_manager;

	if (bms == NULL || bms->ext_cache_cnt >= WT_BLOCK_MGR_CACHE_EXT_MAX)
		__wt_free(session, ext);
	else {
		ext->next[0] = bms->ext_cache;
		bms->ext_cache = ext;

		++bms->ext_cache_cnt;
	}
}

/*
 * __block_ext_cleanup --
 *	Cleanup the WT_EXT structure cache.
 */
static int
__block_ext_cleanup(WT_SESSION_IMPL *session, WT_BLOCK_MGR_SESSION *bms)
{
	WT_EXT *ext, *next;

	for (ext = bms->ext_cache;
	    ext != NULL; ext = next, --bms->ext_cache_cnt) {
		next = ext->next[0];
		__wt_free(session, ext);
	}
	if (bms->ext_cache_cnt != 0)
		WT_RET_MSG(session, WT_ERROR,
		    "incorrect count in session handle's block manager cache");
	return (0);
}

/*
 * __block_size_alloc --
 *	Allocate a new WT_SIZE structure.
 */
static int
__block_size_alloc(WT_SESSION_IMPL *session, WT_SIZE **szp)
{
	return (__wt_calloc(session, 1, sizeof(WT_SIZE), szp));
}

/*
 * __wt_block_size_alloc --
 *	Allocate WT_SIZE structures.
 */
int
__wt_block_size_alloc(WT_SESSION_IMPL *session, WT_SIZE **szp, u_int max)
{
	WT_BLOCK_MGR_SESSION *bms;
	WT_SIZE *sz;

	bms = session->block_manager;

	/* First, return a WT_SIZE structure for use from a cached list. */
	if (szp != NULL && bms != NULL && bms->sz_cache != NULL) {
		(*szp) = bms->sz_cache;
		bms->sz_cache = bms->sz_cache->next[0];

		/*
		 * The count is advisory to minimize our exposure to bugs, but
		 * don't let it go negative, that would lead to never caching
		 * a WT_SIZE structure again, we'd always believe there are too
		 * many cached.
		 */
		if (bms->sz_cache_cnt > 0)
			--bms->sz_cache_cnt;
		return (0);
	}

	/* Second, allocating a WT_SIZE structure for use. */
	if (szp != NULL) {
		WT_STAT_FAST_CONN_INCR(session, block_locked_allocation);
		return (__block_size_alloc(session, szp));
	}

	/* Third, pre-allocating WT_SIZE structures for later use. */
	if (bms == NULL) {
		WT_RET(__wt_calloc(session,
		    1, sizeof(WT_BLOCK_MGR_SESSION), &session->block_manager));
		bms = session->block_manager;
		session->block_manager_cleanup =
		    __block_manager_session_cleanup;
	}
	for (; bms->sz_cache_cnt < max; ++bms->sz_cache_cnt) {
		WT_RET(__block_size_alloc(session, &sz));

		sz->next[0] = bms->sz_cache;
		bms->sz_cache = sz;
	}
	return (0);
}

/*
 * __wt_block_size_free --
 *	Add an WT_SIZE structure to the cached list, or free if enough cached.
 */
void
__wt_block_size_free(WT_SESSION_IMPL *session, WT_SIZE *sz)
{
	WT_BLOCK_MGR_SESSION *bms;

	bms = session->block_manager;

	if (bms == NULL || bms->sz_cache_cnt >= WT_BLOCK_MGR_CACHE_SIZE_MAX)
		__wt_free(session, sz);
	else {
		sz->next[0] = bms->sz_cache;
		bms->sz_cache = sz;

		++bms->sz_cache_cnt;
	}
}

/*
 * __block_size_cleanup --
 *	Cleanup the WT_SIZE structure cache.
 */
static int
__block_size_cleanup(WT_SESSION_IMPL *session, WT_BLOCK_MGR_SESSION *bms)
{
	WT_SIZE *sz, *nsz;

	for (sz = bms->sz_cache; sz != NULL; sz = nsz, --bms->sz_cache_cnt) {
		nsz = sz->next[0];
		__wt_free(session, sz);
	}
	if (bms->sz_cache_cnt != 0)
		WT_RET_MSG(session, WT_ERROR,
		    "incorrect count in session handle's block manager cache");
	return (0);
}

/*
 * __block_manager_session_cleanup --
 *	Clean up the session handle's block manager information.
 */
static int
__block_manager_session_cleanup(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;

	if (session->block_manager == NULL)
		return (0);

	WT_TRET(__block_ext_cleanup(session, session->block_manager));
	WT_TRET(__block_size_cleanup(session, session->block_manager));

	__wt_free(session, session->block_manager);

	return (ret);
}
