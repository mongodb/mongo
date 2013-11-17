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
	WT_EXT *ext_cache;			/* List of WT_EXT handles */
	u_int   ext_cache_cnt;			/* Count */
} WT_BLOCK_MGR_SESSION;

/*
 * __block_manager_session_cleanup --
 *	Clean up the session handle's block manager information.
 */
static int
__block_manager_session_cleanup(WT_SESSION_IMPL *session)
{
	WT_BLOCK_MGR_SESSION *bms;
	WT_DECL_RET;
	WT_EXT *ext, *next;

	if ((bms = session->block_manager) == NULL)
		return (0);

	for (ext = bms->ext_cache;
	    ext != NULL; ext = next, --bms->ext_cache_cnt) {
		next = ext->next[0];
		__wt_free(session, ext);
	}

	if (bms->ext_cache_cnt != 0) {
		ret = WT_ERROR;
		__wt_errx(session,
		    "incorrect count in session handle's block manager cache");
	}
	__wt_free(session, session->block_manager);

	return (ret);
}

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
		 * don't let it go negative, that would lead to never cacheing
		 * a WT_EXT structure  again, we'd always believe there are too
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
	if (bms->ext_cache_cnt >= max)
		return (0);
	do {
		WT_RET(__block_ext_alloc(session, &ext));

		ext->next[0] = bms->ext_cache;
		bms->ext_cache = ext;
	} while (++bms->ext_cache_cnt < max);
	return (0);
}

/*
 * __wt_block_ext_free --
 *	Add an EXT structure to the cached list, or free it if enough cached.
 */
void
__wt_block_ext_free(WT_SESSION_IMPL *session, WT_EXT *ext)
{
	WT_BLOCK_MGR_SESSION *bms;

	bms = session->block_manager;

	if (bms == NULL || bms->ext_cache_cnt > 100)
		__wt_free(session, ext);
	else {
		ext->next[0] = bms->ext_cache;
		bms->ext_cache = ext;

		++bms->ext_cache_cnt;
	}
}
