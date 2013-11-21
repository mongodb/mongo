/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Compaction is the place where the underlying block manager becomes visible
 * in the higher engine btree and API layers.  As there is currently only one
 * block manager, this code is written with it in mind: other block managers
 * may need changes to support compaction, and a smart block manager might need
 * far less support from the engine.
 *
 * First, the default block manager cannot entirely own compaction because it
 * has no way to find a block after it moves other than a request from the
 * btree layer with the new address.  In other words, if internal page X points
 * to leaf page Y, and page Y moves, the address of page Y has to be updated in
 * page X.  Generally, this is solved by building a translation layer in the
 * block manager so internal pages don't require updates to relocate blocks:
 * however, the translation table must be durable, has its own garbage
 * collection issues and might be slower, all of which have their own problems.
 *
 * Second, the btree layer cannot entirely own compaction because page
 * addresses are opaque, it cannot know where a page is in the file from the
 * address cookie.
 *
 * For these reasons, compaction is a cooperative process between the btree
 * layer and the block manager.  The btree layer walks files, and asks the
 * block manager if rewriting a particular block would reduce the file
 * footprint: if writing the page will help, the page is marked dirty so it
 * will eventually be written.  As pages are written, the original page
 * potentially becomes available for reuse and if enough pages at the end of
 * the file are available for reuse, the file can be truncated, and compaction
 * succeeds.
 *
 * However, writing a page is not by itself sufficient to make a page available
 * for reuse.  The original version of the page is still referenced by at least
 * the most recent checkpoint in the file.  To make a page available for reuse,
 * we have to checkpoint the file so we can discard the checkpoint referencing
 * the original version of the block; once no checkpoint references a block, it
 * becomes available for reuse.
 *
 * Compaction is not necessarily possible in WiredTiger, even in a file with
 * lots of available space.  If a block at the end of the file is referenced by
 * a named checkpoint, there is nothing we can do to compact the file, no
 * matter how many times we rewrite the block, the named checkpoint can't be
 * discarded and so the reference count on the original block will never go to
 * zero.   What's worse, because the block manager doesn't reference count
 * blocks, it can't easily know this is the case, and so we'll waste a lot of
 * effort trying to compact files that can't be compacted.
 *
 * Now, to the actual process.  First, we checkpoint the high-level object
 * (which is potentially composed of multiple files): there are potentially
 * many dirty blocks in the cache, and we want to write them out and then
 * discard previous checkpoints so we have as many blocks as possible on the
 * file's "available for reuse" list when we start compaction.
 *
 * Then, we compact the high-level object.
 *
 * Compacting the object is done 10% at a time, that is, we try and move blocks
 * from the last 10% of the file into the beginning of the file (the 10% is
 * hard coded in the block manager).  The reason for this is because we are
 * walking the file in logical order, not block offset order, and we can fail
 * to compact a file if we write the wrong blocks first.
 *
 * For example, imagine a file with 10 blocks in the first 10% of a file, 1,000
 * blocks in the 3rd quartile of the file, and 10 blocks in the last 10% of the
 * file.  If we were to rewrite blocks from more than the last 10% of the file,
 * and found the 1,000 blocks in the 3rd quartile of the file first, we'd copy
 * 10 of them without ever rewriting the blocks from the end of the file which
 * would allow us to compact the file.  So, we compact the last 10% of the
 * file, and if that works, we compact the last 10% of the file again, and so
 * on.  Note the block manager uses a first-fit block selection algorithm
 * during compaction to maximize block movement.
 *
 * After each 10% compaction, we checkpoint two more times (seriously, twice).
 * The second and third checkpoints are because the block manager checkpoints
 * in two steps: blocks made available for reuse during a checkpoint are put on
 *
 * a special checkpoint-available list and only moved to the real available
 * list after the metadata has been updated with the new checkpoint's
 * information.  (Otherwise it is possible to allocate a rewritten block, crash
 * before the metadata is updated, and see corruption.)  For this reason,
 * blocks allocated to write the checkpoint itself cannot be taken from the
 * blocks made available by the checkpoint.
 *
 * To say it another way, the second checkpoint puts the blocks from the end of
 * the file that were made available by compaction onto the
 * checkpoint-available list, but then potentially writes the checkpoint itself
 * at the end of the file, which would prevent any file truncation.  When the
 * metadata is updated for the second checkpoint, the blocks freed by
 * compaction become available for the third checkpoint, so the third
 * checkpoint's blocks are written towards the beginning of the file, and then
 * the file can be truncated.
 */

/*
 * __session_compact_worker --
 *	Worker function to do the actual compaction call.
 */
static int
__session_compact_worker(
    WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	SESSION_API_CALL(session, compact, config, cfg);

	WT_WITH_SCHEMA_LOCK(session,
	    ret = __wt_schema_worker(session, uri, __wt_compact, NULL, cfg, 0));

err:	API_END_NOTFOUND_MAP(session, ret);
}

/*
 * __wt_session_compact --
 *	Function to alternate between checkpoints and compaction calls.
 */
int
__wt_session_compact(
    WT_SESSION *wt_session, const char *uri, const char *config)
{
	WT_DECL_ITEM(t);
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	int i;

	session = (WT_SESSION_IMPL *)wt_session;

	/*
	 * Force the checkpoint: we don't want to skip it because the work we
	 * need to have done is done in the underlying block manager.
	 */
	WT_RET(__wt_scr_alloc(session, 128, &t));
	WT_ERR(__wt_buf_fmt(session, t, "target=(\"%s\"),force=1", uri));

	/*
	 * We compact 10% of the file on each pass, try 10 times (which is
	 * probably overkill), and quit if we make no progress.
	 */
	for (i = 0; i < 10; ++i) {
		WT_ERR(wt_session->checkpoint(wt_session, t->data));

		session->compaction = 0;
		WT_ERR(__session_compact_worker(wt_session, uri, config));
		if (!session->compaction)
			break;

		WT_ERR(wt_session->checkpoint(wt_session, t->data));
		WT_ERR(wt_session->checkpoint(wt_session, t->data));
	}

err:	__wt_scr_free(&t);
	return (ret);
}
