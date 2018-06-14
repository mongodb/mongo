/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __checkpoint_timing_stress(WT_SESSION_IMPL *);
static int __checkpoint_lock_dirty_tree(
    WT_SESSION_IMPL *, bool, bool, bool, const char *[]);
static int __checkpoint_mark_skip(WT_SESSION_IMPL *, WT_CKPT *, bool);
static int __checkpoint_presync(WT_SESSION_IMPL *, const char *[]);
static int __checkpoint_tree_helper(WT_SESSION_IMPL *, const char *[]);

/*
 * __checkpoint_name_ok --
 *	Complain if the checkpoint name isn't acceptable.
 */
static int
__checkpoint_name_ok(WT_SESSION_IMPL *session, const char *name, size_t len)
{
	/* Check for characters we don't want to see in a metadata file. */
	WT_RET(__wt_name_check(session, name, len));

	/*
	 * The internal checkpoint name is special, applications aren't allowed
	 * to use it.  Be aggressive and disallow any matching prefix, it makes
	 * things easier when checking in other places.
	 */
	if (len < strlen(WT_CHECKPOINT))
		return (0);
	if (!WT_PREFIX_MATCH(name, WT_CHECKPOINT))
		return (0);

	WT_RET_MSG(session, EINVAL,
	    "the checkpoint name \"%s\" is reserved", WT_CHECKPOINT);
}

/*
 * __checkpoint_name_check --
 *	Check for an attempt to name a checkpoint that includes anything
 * other than a file object.
 */
static int
__checkpoint_name_check(WT_SESSION_IMPL *session, const char *uri)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	const char *fail;

	cursor = NULL;
	fail = NULL;

	/*
	 * This function exists as a place for this comment: named checkpoints
	 * are only supported on file objects, and not on LSM trees or Helium
	 * devices.  If a target list is configured for the checkpoint, this
	 * function is called with each target list entry; check the entry to
	 * make sure it's backed by a file.  If no target list is configured,
	 * confirm the metadata file contains no non-file objects. Skip any
	 * internal system objects. We don't want spurious error messages,
	 * other code will skip over them and the user has no control over
	 * their existence.
	 */
	if (uri == NULL) {
		WT_RET(__wt_metadata_cursor(session, &cursor));
		while ((ret = cursor->next(cursor)) == 0) {
			WT_ERR(cursor->get_key(cursor, &uri));
			if (!WT_PREFIX_MATCH(uri, "colgroup:") &&
			    !WT_PREFIX_MATCH(uri, "file:") &&
			    !WT_PREFIX_MATCH(uri, "index:") &&
			    !WT_PREFIX_MATCH(uri, WT_SYSTEM_PREFIX) &&
			    !WT_PREFIX_MATCH(uri, "table:")) {
				fail = uri;
				break;
			}
		}
		WT_ERR_NOTFOUND_OK(ret);
	} else
		if (!WT_PREFIX_MATCH(uri, "colgroup:") &&
		    !WT_PREFIX_MATCH(uri, "file:") &&
		    !WT_PREFIX_MATCH(uri, "index:") &&
		    !WT_PREFIX_MATCH(uri, "table:"))
			fail = uri;

	if (fail != NULL)
		WT_ERR_MSG(session, EINVAL,
		    "%s object does not support named checkpoints", fail);

err:	WT_TRET(__wt_metadata_cursor_release(session, &cursor));
	return (ret);
}

/*
 * __checkpoint_update_generation --
 *	Update the checkpoint generation of the current tree.
 *
 *	This indicates that the tree will not be visited again by the current
 *	checkpoint.
 */
static void
__checkpoint_update_generation(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	/*
	 * Updates to the metadata are made by the checkpoint transaction, so
	 * the metadata tree's checkpoint generation should never be updated.
	 */
	if (WT_IS_METADATA(session->dhandle))
		return;

	WT_PUBLISH(btree->checkpoint_gen, __wt_gen(session, WT_GEN_CHECKPOINT));
	WT_STAT_DATA_SET(session,
	    btree_checkpoint_generation, btree->checkpoint_gen);
}

/*
 * __checkpoint_apply_all --
 *	Apply an operation to all files involved in a checkpoint.
 */
static int
__checkpoint_apply_all(WT_SESSION_IMPL *session, const char *cfg[],
    int (*op)(WT_SESSION_IMPL *, const char *[]))
{
	WT_CONFIG targetconf;
	WT_CONFIG_ITEM cval, k, v;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	bool ckpt_closed, named, target_list;

	target_list = false;

	/* Flag if this is a named checkpoint, and check if the name is OK. */
	WT_RET(__wt_config_gets(session, cfg, "name", &cval));
	named = cval.len != 0;
	if (named)
		WT_RET(__checkpoint_name_ok(session, cval.str, cval.len));

	/* Step through the targets and optionally operate on each one. */
	WT_ERR(__wt_config_gets(session, cfg, "target", &cval));
	__wt_config_subinit(session, &targetconf, &cval);
	while ((ret = __wt_config_next(&targetconf, &k, &v)) == 0) {
		if (!target_list) {
			WT_ERR(__wt_scr_alloc(session, 512, &tmp));
			target_list = true;
		}

		if (v.len != 0)
			WT_ERR_MSG(session, EINVAL,
			    "invalid checkpoint target %.*s: URIs may require "
			    "quoting",
			    (int)cval.len, (char *)cval.str);

		/* Some objects don't support named checkpoints. */
		if (named)
			WT_ERR(__checkpoint_name_check(session, k.str));

		if (op == NULL)
			continue;
		WT_ERR(__wt_buf_fmt(session, tmp, "%.*s", (int)k.len, k.str));
		if ((ret = __wt_schema_worker(
		    session, tmp->data, op, NULL, cfg, 0)) != 0)
			WT_ERR_MSG(session, ret, "%s", (const char *)tmp->data);
	}
	WT_ERR_NOTFOUND_OK(ret);

	if (!target_list && named)
		/* Some objects don't support named checkpoints. */
		WT_ERR(__checkpoint_name_check(session, NULL));

	if (!target_list && op != NULL) {
		/*
		 * If the checkpoint is named or we're dropping checkpoints, we
		 * checkpoint both open and closed files; else, only checkpoint
		 * open files.
		 *
		 * XXX
		 * We don't optimize unnamed checkpoints of a list of targets,
		 * we open the targets and checkpoint them even if they are
		 * quiescent and don't need a checkpoint, believing applications
		 * unlikely to checkpoint a list of closed targets.
		 */
		ckpt_closed = named;
		if (!ckpt_closed) {
			WT_ERR(__wt_config_gets(session, cfg, "drop", &cval));
			ckpt_closed = cval.len != 0;
		}
		WT_ERR(ckpt_closed ?
		    __wt_meta_apply_all(session, op, NULL, cfg) :
		    __wt_conn_btree_apply(session, NULL, op, NULL, cfg));
	}

err:	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
 * __checkpoint_apply --
 *	Apply an operation to all handles locked for a checkpoint.
 */
static int
__checkpoint_apply(WT_SESSION_IMPL *session, const char *cfg[],
    int (*op)(WT_SESSION_IMPL *, const char *[]))
{
	WT_DECL_RET;
	u_int i;

	/* If we have already locked the handles, apply the operation. */
	for (i = 0; i < session->ckpt_handle_next; ++i) {
		if (session->ckpt_handle[i] == NULL)
			continue;
		WT_WITH_DHANDLE(session, session->ckpt_handle[i],
		    ret = (*op)(session, cfg));
		WT_RET(ret);
	}

	return (0);
}

/*
 * __checkpoint_data_source --
 *	Checkpoint all data sources.
 */
static int
__checkpoint_data_source(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DATA_SOURCE *dsrc;
	WT_NAMED_DATA_SOURCE *ndsrc;

	/*
	 * A place-holder, to support Helium devices: we assume calling the
	 * underlying data-source session checkpoint function is sufficient to
	 * checkpoint all objects in the data source, open or closed, and we
	 * don't attempt to optimize the checkpoint of individual targets.
	 * Those assumptions is correct for the Helium device, but it's not
	 * necessarily going to be true for other data sources.
	 *
	 * It's not difficult to support data-source checkpoints of individual
	 * targets (__wt_schema_worker is the underlying function that will do
	 * the work, and it's already written to support data-sources, although
	 * we'd probably need to pass the URI of the object to the data source
	 * checkpoint function which we don't currently do).  However, doing a
	 * full data checkpoint is trickier: currently, the connection code is
	 * written to ignore all objects other than "file:", and that code will
	 * require significant changes to work with data sources.
	 */
	TAILQ_FOREACH(ndsrc, &S2C(session)->dsrcqh, q) {
		dsrc = ndsrc->dsrc;
		if (dsrc->checkpoint != NULL)
			WT_RET(dsrc->checkpoint(dsrc,
			    (WT_SESSION *)session, (WT_CONFIG_ARG *)cfg));
	}
	return (0);
}

/*
 * __wt_checkpoint_get_handles --
 *	Get a list of handles to flush.
 */
int
__wt_checkpoint_get_handles(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	const char *name;
	bool force;

	/* Find out if we have to force a checkpoint. */
	WT_RET(__wt_config_gets_def(session, cfg, "force", 0, &cval));
	force = cval.val != 0;
	if (!force) {
		WT_RET(__wt_config_gets_def(session, cfg, "name", 0, &cval));
		force = cval.len != 0;
	}

	/* Should not be called with anything other than a live btree handle. */
	WT_ASSERT(session, session->dhandle->type == WT_DHANDLE_TYPE_BTREE &&
	    session->dhandle->checkpoint == NULL);

	btree = S2BT(session);

	/* Skip files that are never involved in a checkpoint. */
	if (F_ISSET(btree, WT_BTREE_NO_CHECKPOINT))
		return (0);

	/*
	 * We may have raced between starting the checkpoint transaction and
	 * some operation completing on the handle that updated the metadata
	 * (e.g., closing a bulk load cursor).  All such operations either have
	 * exclusive access to the handle or hold the schema lock.  We are now
	 * holding the schema lock and have an open btree handle, so if we
	 * can't update the metadata, then there has been some state change
	 * invisible to the checkpoint transaction.
	 */
	if (!WT_IS_METADATA(session->dhandle)) {
		WT_CURSOR *meta_cursor;

		WT_ASSERT(session, !F_ISSET(&session->txn, WT_TXN_ERROR));
		WT_RET(__wt_metadata_cursor(session, &meta_cursor));
		meta_cursor->set_key(meta_cursor, session->dhandle->name);
		ret = __wt_curfile_insert_check(meta_cursor);
		if (ret == WT_ROLLBACK) {
			/*
			 * If create or drop or any schema operation of a table
			 * is with in an user transaction then checkpoint can
			 * see the dhandle before the commit, which will lead
			 * to the rollback error. We will ignore this dhandle as
			 * part of this checkpoint by returning from here.
			 */
			WT_TRET(__wt_metadata_cursor_release(session,
			    &meta_cursor));
			return (0);
		}
		WT_TRET(__wt_metadata_cursor_release(session, &meta_cursor));
		WT_RET(ret);
	}

	/*
	 * Decide whether the tree needs to be included in the checkpoint and
	 * if so, acquire the necessary locks.
	 */
	WT_SAVE_DHANDLE(session, ret = __checkpoint_lock_dirty_tree(
	    session, true, force, true, cfg));
	WT_RET(ret);
	if (F_ISSET(btree, WT_BTREE_SKIP_CKPT)) {
		WT_ASSERT(session, btree->ckpt == NULL);
		__checkpoint_update_generation(session);
		return (0);
	}

	/*
	 * Make sure there is space for the new entry: do this before getting
	 * the handle to avoid cleanup if we can't allocate the memory.
	 */
	WT_RET(__wt_realloc_def(session, &session->ckpt_handle_allocated,
	    session->ckpt_handle_next + 1, &session->ckpt_handle));

	/*
	 * The current tree will be included: get it again because the handle
	 * we have is only valid for the duration of this function.
	 */
	name = session->dhandle->name;
	session->dhandle = NULL;

	if ((ret = __wt_session_get_dhandle(session, name, NULL, NULL, 0)) != 0)
		return (ret == EBUSY ? 0 : ret);

	/*
	 * Save the current eviction walk setting: checkpoint can interfere
	 * with eviction and we don't want to unfairly penalize (or promote)
	 * eviction in trees due to checkpoints.
	 */
	btree->evict_walk_saved = btree->evict_walk_period;

	session->ckpt_handle[session->ckpt_handle_next++] = session->dhandle;
	return (0);
}

/*
 * __checkpoint_reduce_dirty_cache --
 *	Release clean trees from the list cached for checkpoints.
 */
static void
__checkpoint_reduce_dirty_cache(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	double current_dirty, delta, scrub_min;
	uint64_t bytes_written_last, bytes_written_start, bytes_written_total;
	uint64_t cache_size, max_write;
	uint64_t current_us, stepdown_us, total_ms, work_us;
	uint64_t time_last, time_start, time_stop;
	bool progress;

	conn = S2C(session);
	cache = conn->cache;

	/*
	 * Give up if scrubbing is disabled, including when checkpointing with
	 * a timestamp on close (we can't evict dirty pages in that case, so
	 * scrubbing cannot help).
	 */
	if (F_ISSET(conn, WT_CONN_CLOSING_TIMESTAMP) ||
	    cache->eviction_checkpoint_target < DBL_EPSILON ||
	    cache->eviction_checkpoint_target >= cache->eviction_dirty_trigger)
		return;

	time_last = time_start = __wt_clock(session);
	bytes_written_last = 0;
	bytes_written_start = cache->bytes_written;
	cache_size = conn->cache_size;
	/*
	 * If the cache size is zero or very small, we're done.  The cache
	 * size can briefly become zero if we're transitioning to a shared
	 * cache via reconfigure.  This avoids potential divide by zero.
	 */
	if (cache_size < 10 * WT_MEGABYTE)
		return;

	/*
	 * Skip scrubbing if it won't perform at-least some minimum amount of
	 * work. Scrubbing is supposed to bring down the dirty data to eviction
	 * checkpoint target before the actual checkpoint starts. Do not perform
	 * scrubbing if the dirty data to scrub is less than a pre-configured
	 * size. This size is to an extent based on the configured cache size
	 * without being too large or too small for large cache sizes. For the
	 * values chosen, for instance, 100 GB cache will require at-least
	 * 200 MB of dirty data above eviction checkpoint target, which should
	 * equate to a scrub phase a few seconds long. That said, the value of
	 * 0.2% and 500 MB are still somewhat arbitrary.
	 */
	scrub_min = WT_MIN((0.2 * conn->cache_size) / 100, 500 * WT_MEGABYTE);
	if (__wt_cache_dirty_leaf_inuse(cache) <
	    ((cache->eviction_checkpoint_target * conn->cache_size) / 100) +
	    scrub_min)
		return;

	stepdown_us = 10000;
	work_us = 0;
	progress = false;

	/* Step down the scrub target (as a percentage) in units of 10MB. */
	delta = WT_MIN(1.0, (100 * 10.0 * WT_MEGABYTE) / cache_size);

	/*
	 * Start with the scrub target equal to the expected maximum percentage
	 * of dirty data in cache.
	 */
	cache->eviction_scrub_limit = cache->eviction_dirty_trigger;

	/* Stop if we write as much dirty data as is currently in cache. */
	max_write = __wt_cache_dirty_leaf_inuse(cache);

	/* Step down the dirty target to the eviction trigger */
	for (;;) {
		current_dirty =
		    (100.0 * __wt_cache_dirty_leaf_inuse(cache)) / cache_size;
		if (current_dirty <= cache->eviction_checkpoint_target)
			break;

		/*
		 * Don't scrub when the lookaside table is in use: scrubbing is
		 * counter-productive in that case.
		 */
		if (F_ISSET(cache, WT_CACHE_EVICT_LOOKASIDE))
			break;

		__wt_sleep(0, stepdown_us / 10);
		time_stop = __wt_clock(session);
		current_us = WT_CLOCKDIFF_US(time_stop, time_last);
		bytes_written_total =
		    cache->bytes_written - bytes_written_start;

		if (current_dirty > cache->eviction_scrub_limit) {
			/*
			 * We haven't reached the current target.
			 *
			 * Don't wait indefinitely: there might be dirty pages
			 * that can't be evicted.  If we can't meet the target,
			 * give up and start the checkpoint for real.
			 */
			if (current_us > WT_MAX(WT_MILLION, 10 * stepdown_us) ||
			    bytes_written_total > max_write)
				break;
			continue;
		}

		/*
		 * Estimate how long the next step down of dirty data should
		 * take.
		 *
		 * The calculation here assumes that the system is writing from
		 * cache as fast as it can, and determines the write throughput
		 * based on the change in the bytes written from cache since
		 * the start of the call.  We use that to estimate how long it
		 * will take to step the dirty target down by delta.
		 *
		 * Take care to avoid dividing by zero.
		 */
		if (bytes_written_total - bytes_written_last > WT_MEGABYTE &&
		    work_us > 0) {
			stepdown_us = (uint64_t)((delta * cache_size / 100) /
			    ((double)bytes_written_total / work_us));
			stepdown_us = WT_MAX(1, stepdown_us);
			if (!progress)
				stepdown_us = WT_MIN(stepdown_us, 200000);
			progress = true;

			bytes_written_last = bytes_written_total;
		}

		work_us += current_us;

		/*
		 * Smooth out step down: try to limit the impact on
		 * performance to 10% by waiting once we reach the last
		 * level.
		 */
		__wt_sleep(0, 10 * stepdown_us);
		cache->eviction_scrub_limit =
		    WT_MAX(cache->eviction_dirty_target, current_dirty - delta);
		WT_STAT_CONN_SET(session, txn_checkpoint_scrub_target,
		    cache->eviction_scrub_limit);
		time_last = __wt_clock(session);
	}

	time_stop = __wt_clock(session);
	total_ms = WT_CLOCKDIFF_MS(time_stop, time_start);
	WT_STAT_CONN_SET(session, txn_checkpoint_scrub_time, total_ms);
}

/*
 * __wt_checkpoint_progress --
 * 	Output a checkpoint progress message.
 */
void
__wt_checkpoint_progress(WT_SESSION_IMPL *session, bool closing)
{
	struct timespec cur_time;
	WT_CONNECTION_IMPL *conn;
	uint64_t time_diff;

	conn = S2C(session);
	__wt_epoch(session, &cur_time);

	/* Time since the full database checkpoint started */
	time_diff = WT_TIMEDIFF_SEC(cur_time,
	    conn->ckpt_timer_start);

	if (closing || (time_diff / 20) > conn->ckpt_progress_msg_count) {
		__wt_verbose(session, WT_VERB_CHECKPOINT_PROGRESS,
		    "Checkpoint %s for %" PRIu64
		    " seconds and wrote: %" PRIu64 " pages (%" PRIu64 " MB)",
		    closing ? "ran" : "has been running",
		    time_diff, conn->ckpt_write_pages,
		    conn->ckpt_write_bytes / WT_MEGABYTE);
		conn->ckpt_progress_msg_count++;
	}
}

/*
 * __checkpoint_stats --
 *	Update checkpoint timer stats.
 */
static void
__checkpoint_stats(WT_SESSION_IMPL *session)
{
	struct timespec stop;
	WT_CONNECTION_IMPL *conn;
	uint64_t msec;

	conn = S2C(session);

	/* Output a verbose progress message for long running checkpoints */
	if (conn->ckpt_progress_msg_count > 0)
		__wt_checkpoint_progress(session, true);

	__wt_epoch(session, &stop);
	msec = WT_TIMEDIFF_MS(stop, conn->ckpt_timer_scrub_end);

	if (msec > conn->ckpt_time_max)
		conn->ckpt_time_max = msec;
	if (conn->ckpt_time_min == 0 || msec < conn->ckpt_time_min)
		conn->ckpt_time_min = msec;
	conn->ckpt_time_recent = msec;
	conn->ckpt_time_total += msec;
}

/*
 * __checkpoint_verbose_track --
 *	Output a verbose message with timing information
 */
static void
__checkpoint_verbose_track(WT_SESSION_IMPL *session, const char *msg)
{
	struct timespec stop;
	WT_CONNECTION_IMPL *conn;
	uint64_t msec;

	if (!WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT))
		return;

	conn = S2C(session);
	__wt_epoch(session, &stop);

	/* Get time diff in milliseconds. */
	msec = WT_TIMEDIFF_MS(stop, conn->ckpt_timer_start);
	__wt_verbose(session,
	    WT_VERB_CHECKPOINT, "time: %" PRIu64 " ms, gen: %" PRIu64
	    ": Full database checkpoint %s",
	    msec, __wt_gen(session, WT_GEN_CHECKPOINT), msg);

}

/*
 * __checkpoint_fail_reset --
 *	Reset fields when a failure occurs.
 */
static void
__checkpoint_fail_reset(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;

	btree = S2BT(session);
	btree->modified = true;
	__wt_meta_ckptlist_free(session, &btree->ckpt);
}

/*
 * __checkpoint_prepare --
 *	Start the transaction for a checkpoint and gather handles.
 */
static int
__checkpoint_prepare(
    WT_SESSION_IMPL *session, bool *trackingp, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;
	const char *txn_cfg[] = { WT_CONFIG_BASE(session,
	    WT_SESSION_begin_transaction), "isolation=snapshot", NULL };
	bool use_timestamp;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;
	txn_state = WT_SESSION_TXN_STATE(session);

	WT_RET(__wt_config_gets(session, cfg, "use_timestamp", &cval));
	use_timestamp = (cval.val != 0);

	/*
	 * Start a snapshot transaction for the checkpoint.
	 *
	 * Note: we don't go through the public API calls because they have
	 * side effects on cursors, which applications can hold open across
	 * calls to checkpoint.
	 */
	WT_RET(__wt_txn_begin(session, txn_cfg));

	WT_DIAGNOSTIC_YIELD;

	/* Ensure a transaction ID is allocated prior to sharing it globally */
	WT_RET(__wt_txn_id_check(session));

	/* Keep track of handles acquired for locking. */
	WT_RET(__wt_meta_track_on(session));
	*trackingp = true;

	/*
	 * Mark the connection as clean. If some data gets modified after
	 * generating checkpoint transaction id, connection will be reset to
	 * dirty when reconciliation marks the btree dirty on encountering the
	 * dirty page.
	 */
	conn->modified = false;

	/*
	 * Save the checkpoint session ID.
	 *
	 * We never do checkpoints in the default session (with id zero).
	 */
	WT_ASSERT(session, session->id != 0 && txn_global->checkpoint_id == 0);
	txn_global->checkpoint_id = session->id;

	/*
	 * Remove the checkpoint transaction from the global table.
	 *
	 * This allows ordinary visibility checks to move forward because
	 * checkpoints often take a long time and only write to the metadata.
	 */
	__wt_writelock(session, &txn_global->rwlock);
	txn_global->checkpoint_state = *txn_state;
	txn_global->checkpoint_txn = txn;
	txn_global->checkpoint_state.pinned_id = WT_MIN(txn->id, txn->snap_min);

	/*
	 * Sanity check that the oldest ID hasn't moved on before we have
	 * cleared our entry.
	 */
	WT_ASSERT(session,
	    WT_TXNID_LE(txn_global->oldest_id, txn_state->id) &&
	    WT_TXNID_LE(txn_global->oldest_id, txn_state->pinned_id));

	/*
	 * Clear our entry from the global transaction session table. Any
	 * operation that needs to know about the ID for this checkpoint will
	 * consider the checkpoint ID in the global structure. Most operations
	 * can safely ignore the checkpoint ID (see the visible all check for
	 * details).
	 */
	txn_state->id = txn_state->pinned_id =
	    txn_state->metadata_pinned = WT_TXN_NONE;

#ifdef HAVE_TIMESTAMPS
	/*
	 * Set the checkpoint transaction's timestamp, if requested.
	 *
	 * We rely on having the global transaction data locked so the oldest
	 * timestamp can't move past the stable timestamp.
	 */
	WT_ASSERT(session, !F_ISSET(txn,
	    WT_TXN_HAS_TS_COMMIT | WT_TXN_HAS_TS_READ |
	    WT_TXN_PUBLIC_TS_COMMIT | WT_TXN_PUBLIC_TS_READ));

	if (use_timestamp) {
		/*
		 * If the user wants timestamps then set the metadata
		 * checkpoint timestamp based on whether or not a stable
		 * timestamp is actually in use.  Only set it when we're not
		 * running recovery because recovery doesn't set the recovery
		 * timestamp until its checkpoint is complete.
		 */
		if (txn_global->has_stable_timestamp) {
			__wt_timestamp_set(&txn->read_timestamp,
			    &txn_global->stable_timestamp);
			F_SET(txn, WT_TXN_HAS_TS_READ);
			if (!F_ISSET(conn, WT_CONN_RECOVERING))
				__wt_timestamp_set(
				    &txn_global->meta_ckpt_timestamp,
				    &txn->read_timestamp);
		} else if (!F_ISSET(conn, WT_CONN_RECOVERING))
			__wt_timestamp_set(&txn_global->meta_ckpt_timestamp,
			    &txn_global->recovery_timestamp);
	} else if (!F_ISSET(conn, WT_CONN_RECOVERING))
		__wt_timestamp_set_zero(&txn_global->meta_ckpt_timestamp);
#else
	WT_UNUSED(use_timestamp);
#endif

	__wt_writeunlock(session, &txn_global->rwlock);

#ifdef HAVE_TIMESTAMPS
	if (F_ISSET(txn, WT_TXN_HAS_TS_READ)) {
		__wt_verbose_timestamp(session, &txn->read_timestamp,
		    "Checkpoint requested at stable timestamp");

		/*
		 * The snapshot we established when the transaction started may
		 * be too early to match the timestamp we just read.
		 *
		 * Get a new one.
		 */
		__wt_txn_get_snapshot(session);
	}
#endif

	/*
	 * Get a list of handles we want to flush; for named checkpoints this
	 * may pull closed objects into the session cache.
	 *
	 * First, gather all handles, then start the checkpoint transaction,
	 * then release any clean handles.
	 */
	WT_ASSERT(session, session->ckpt_handle_next == 0);
	WT_WITH_TABLE_READ_LOCK(session, ret = __checkpoint_apply_all(
	    session, cfg, __wt_checkpoint_get_handles));
	return (ret);
}

/*
 * __txn_checkpoint_can_skip --
 *	Determine whether it's safe to skip taking a checkpoint.
 */
static int
__txn_checkpoint_can_skip(WT_SESSION_IMPL *session,
    const char *cfg[], bool *fullp, bool *use_timestampp, bool *can_skipp)
{
	WT_CONFIG targetconf;
	WT_CONFIG_ITEM cval, k, v;
	WT_CONNECTION_IMPL *conn;
	WT_TXN_GLOBAL *txn_global;
	bool full, use_timestamp;

	/*
	 * Default to not skipping - also initialize the other output
	 * parameters - even though they will always be initialized unless
	 * there is an error and callers need to ignore the results on error.
	 */
	*can_skipp = *fullp = *use_timestampp = false;

	conn = S2C(session);
	txn_global = &conn->txn_global;

	/*
	 * This function also parses out some configuration options and hands
	 * them back to the caller - make sure it does that parsing regardless
	 * of the result.
	 *
	 * Determine if this is going to be a full checkpoint, that is a
	 * checkpoint that applies to all data tables in a database.
	 */
	WT_RET(__wt_config_gets(session, cfg, "target", &cval));
	__wt_config_subinit(session, &targetconf, &cval);
	full = __wt_config_next(&targetconf, &k, &v) != 0;
	if (fullp != NULL)
		*fullp = full;

	WT_RET(__wt_config_gets(session, cfg, "use_timestamp", &cval));
	use_timestamp = cval.val != 0;
	if (use_timestampp != NULL)
		*use_timestampp = use_timestamp;

	/* Never skip non-full checkpoints */
	if (!full)
		return (0);

	/* Never skip if force is configured. */
	WT_RET(__wt_config_gets_def(session, cfg, "force", 0, &cval));
	if (cval.val != 0)
		return (0);

	/* Never skip named checkpoints. */
	WT_RET(__wt_config_gets(session, cfg, "name", &cval));
	if (cval.val != 0)
		return (0);

	/*
	 * Skip checkpointing the database if nothing has been dirtied since
	 * the last checkpoint. That said there can be short instances when a
	 * btree gets marked dirty and the connection is yet to be. We might
	 * skip a checkpoint in that short instance, which is okay because by
	 * the next time we get to checkpoint, the connection would have been
	 * marked dirty and hence the checkpoint will not be skipped again.
	 */
	if (!conn->modified) {
		*can_skipp = true;
		return (0);
	}

#ifdef HAVE_TIMESTAMPS
	/*
	 * If the checkpoint is using timestamps, and the stable timestamp
	 * hasn't been updated since the last checkpoint there is nothing
	 * more that could be written.
	 */
	if (use_timestamp && txn_global->has_stable_timestamp &&
	    !__wt_timestamp_iszero(&txn_global->last_ckpt_timestamp) &&
	    __wt_timestamp_cmp(&txn_global->last_ckpt_timestamp,
	    &txn_global->stable_timestamp) == 0) {
		*can_skipp = true;
		return (0);
	}
#else
	WT_UNUSED(txn_global);
#endif

	return (0);
}

/*
 * __txn_checkpoint --
 *	Checkpoint a database or a list of objects in the database.
 */
static int
__txn_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_DECL_TIMESTAMP(ckpt_tmp_ts)
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_ISOLATION saved_isolation;
	uint64_t fsync_duration_usecs, generation, time_start, time_stop;
	u_int i;
	bool can_skip, failed, full, idle, logging, tracking, use_timestamp;
	void *saved_meta_next;

	conn = S2C(session);
	cache = conn->cache;
	txn = &session->txn;
	txn_global = &conn->txn_global;
	saved_isolation = session->isolation;
	full = idle = logging = tracking = use_timestamp = false;

	/* Avoid doing work if possible. */
	WT_RET(__txn_checkpoint_can_skip(session,
	    cfg, &full, &use_timestamp, &can_skip));
	if (can_skip) {
		WT_STAT_CONN_INCR(session, txn_checkpoint_skipped);
		return (0);
	}

	/*
	 * Do a pass over the configuration arguments and figure out what kind
	 * of checkpoint this is.
	 */
	WT_RET(__checkpoint_apply_all(session, cfg, NULL));

	logging = FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED);

	/* Reset the maximum page size seen by eviction. */
	conn->cache->evict_max_page_size = 0;

	/* Initialize the verbose tracking timer */
	__wt_epoch(session, &conn->ckpt_timer_start);

	/* Initialize the checkpoint progress tracking data */
	conn->ckpt_progress_msg_count = 0;
	conn->ckpt_write_bytes = 0;
	conn->ckpt_write_pages = 0;

	/*
	 * Update the global oldest ID so we do all possible cleanup.
	 *
	 * This is particularly important for compact, so that all dirty pages
	 * can be fully written.
	 */
	WT_ERR(__wt_txn_update_oldest(
	    session, WT_TXN_OLDEST_STRICT | WT_TXN_OLDEST_WAIT));

	/* Flush data-sources before we start the checkpoint. */
	WT_ERR(__checkpoint_data_source(session, cfg));

	/*
	 * Try to reduce the amount of dirty data in cache so there is less
	 * work do during the critical section of the checkpoint.
	 */
	__checkpoint_reduce_dirty_cache(session);

	/* Tell logging that we are about to start a database checkpoint. */
	if (full && logging)
		WT_ERR(__wt_txn_checkpoint_log(
		    session, full, WT_TXN_LOG_CKPT_PREPARE, NULL));

	__checkpoint_verbose_track(session, "starting transaction");

	if (full)
		__wt_epoch(session, &conn->ckpt_timer_scrub_end);

	/*
	 * Start the checkpoint for real.
	 *
	 * Bump the global checkpoint generation, used to figure out whether
	 * checkpoint has visited a tree.  Use an atomic increment even though
	 * we are single-threaded because readers of the checkpoint generation
	 * don't hold the checkpoint lock.
	 *
	 * We do need to update it before clearing the checkpoint's entry out
	 * of the transaction table, or a thread evicting in a tree could
	 * ignore the checkpoint's transaction.
	 */
	generation = __wt_gen_next(session, WT_GEN_CHECKPOINT);
	WT_STAT_CONN_SET(session, txn_checkpoint_generation, generation);

	/*
	 * We want to skip checkpointing clean handles whenever possible.  That
	 * is, when the checkpoint is not named or forced.  However, we need to
	 * take care about ordering with respect to the checkpoint transaction.
	 *
	 * We can't skip clean handles before starting the transaction or the
	 * checkpoint can miss updates in trees that become dirty as the
	 * checkpoint is starting.  If we wait until the transaction has
	 * started before locking a handle, there could be a metadata-changing
	 * operation in between (e.g., salvage) that will cause a write
	 * conflict when the checkpoint goes to write the metadata.
	 *
	 * Hold the schema lock while starting the transaction and gathering
	 * handles so the set we get is complete and correct.
	 */
	WT_WITH_SCHEMA_LOCK(session,
	    ret = __checkpoint_prepare(session, &tracking, cfg));
	WT_ERR(ret);

	WT_ASSERT(session, txn->isolation == WT_ISO_SNAPSHOT);

	/*
	 * Unblock updates -- we can figure out that any updates to clean pages
	 * after this point are too new to be written in the checkpoint.
	 */
	cache->eviction_scrub_limit = 0.0;
	WT_STAT_CONN_SET(session, txn_checkpoint_scrub_target, 0);

	/* Tell logging that we have started a database checkpoint. */
	if (full && logging)
		WT_ERR(__wt_txn_checkpoint_log(
		    session, full, WT_TXN_LOG_CKPT_START, NULL));

	__checkpoint_timing_stress(session);

	WT_ERR(__checkpoint_apply(session, cfg, __checkpoint_tree_helper));

	/*
	 * Clear the dhandle so the visibility check doesn't get confused about
	 * the snap min. Don't bother restoring the handle since it doesn't
	 * make sense to carry a handle across a checkpoint.
	 */
	session->dhandle = NULL;

#ifdef HAVE_TIMESTAMPS
	/*
	 * Record the timestamp from the transaction if we were successful.
	 * Store it in a temp variable now because it will be invalidated during
	 * commit but we don't want to set it until we know the checkpoint
	 * is successful. We have to set the system information before we
	 * release the snapshot.
	 */
	__wt_timestamp_set_zero(&ckpt_tmp_ts);
	if (full) {
		WT_ERR(__wt_meta_sysinfo_set(session));
		__wt_timestamp_set(&ckpt_tmp_ts, &txn->read_timestamp);
	}
#endif

	/* Release the snapshot so we aren't pinning updates in cache. */
	__wt_txn_release_snapshot(session);

	/* Mark all trees as open for business (particularly eviction). */
	WT_ERR(__checkpoint_apply(session, cfg, __checkpoint_presync));
	__wt_evict_server_wake(session);

	__checkpoint_verbose_track(session, "committing transaction");

	/*
	 * Checkpoints have to hit disk (it would be reasonable to configure for
	 * lazy checkpoints, but we don't support them yet).
	 */
	time_start = __wt_clock(session);
	WT_ERR(__checkpoint_apply(session, cfg, __wt_checkpoint_sync));
	time_stop = __wt_clock(session);
	fsync_duration_usecs = WT_CLOCKDIFF_US(time_stop, time_start);
	WT_STAT_CONN_INCR(session, txn_checkpoint_fsync_post);
	WT_STAT_CONN_SET(session,
	    txn_checkpoint_fsync_post_duration, fsync_duration_usecs);

	__checkpoint_verbose_track(session, "sync completed");

	/*
	 * Commit the transaction now that we are sure that all files in the
	 * checkpoint have been flushed to disk. It's OK to commit before
	 * checkpointing the metadata since we know that all files in the
	 * checkpoint are now in a consistent state.
	 */
	WT_ERR(__wt_txn_commit(session, NULL));

	/*
	 * Ensure that the metadata changes are durable before the checkpoint
	 * is resolved. Do this by either checkpointing the metadata or syncing
	 * the log file.
	 * Recovery relies on the checkpoint LSN in the metadata only being
	 * updated by full checkpoints so only checkpoint the metadata for
	 * full or non-logged checkpoints.
	 *
	 * This is very similar to __wt_meta_track_off, ideally they would be
	 * merged.
	 */
	if (full || !logging) {
		session->isolation = txn->isolation = WT_ISO_READ_UNCOMMITTED;
		/* Disable metadata tracking during the metadata checkpoint. */
		saved_meta_next = session->meta_track_next;
		session->meta_track_next = NULL;
		WT_WITH_METADATA_LOCK(session,
		    WT_WITH_DHANDLE(session,
			WT_SESSION_META_DHANDLE(session),
			ret = __wt_checkpoint(session, cfg)));
		session->meta_track_next = saved_meta_next;
		WT_ERR(ret);

		WT_WITH_DHANDLE(session,
		    WT_SESSION_META_DHANDLE(session),
		    ret = __wt_checkpoint_sync(session, NULL));
		WT_ERR(ret);

		__checkpoint_verbose_track(session, "metadata sync completed");
	} else
		WT_WITH_DHANDLE(session,
		    WT_SESSION_META_DHANDLE(session),
		    ret = __wt_txn_checkpoint_log(
		    session, false, WT_TXN_LOG_CKPT_SYNC, NULL));

	/*
	 * Now that the metadata is stable, re-open the metadata file for
	 * regular eviction by clearing the checkpoint_pinned flag.
	 */
	txn_global->checkpoint_state.pinned_id = WT_TXN_NONE;

	if (full) {
		__checkpoint_stats(session);
#ifdef HAVE_TIMESTAMPS
		/*
		 * If timestamps were used to define the content of the
		 * checkpoint update the saved last checkpoint timestamp,
		 * otherwise leave it alone. If a checkpoint is taken without
		 * timestamps, it's likely a bug, but we don't want to clear
		 * the saved last checkpoint timestamp regardless.
		 */
		if (use_timestamp)
			__wt_timestamp_set(
			    &conn->txn_global.last_ckpt_timestamp,
			    &ckpt_tmp_ts);
#endif
	}

err:	/*
	 * Reset the timer so that next checkpoint tracks the progress only if
	 * configured.
	 */
	conn->ckpt_timer_start.tv_sec = 0;

	/*
	 * XXX
	 * Rolling back the changes here is problematic.
	 *
	 * If we unroll here, we need a way to roll back changes to the avail
	 * list for each tree that was successfully synced before the error
	 * occurred.  Otherwise, the next time we try this operation, we will
	 * try to free an old checkpoint again.
	 *
	 * OTOH, if we commit the changes after a failure, we have partially
	 * overwritten the checkpoint, so what ends up on disk is not
	 * consistent.
	 */
	failed = ret != 0;
	if (failed)
		conn->modified = true;

	session->isolation = txn->isolation = WT_ISO_READ_UNCOMMITTED;
	if (tracking)
		WT_TRET(__wt_meta_track_off(session, false, failed));

	cache->eviction_scrub_limit = 0.0;
	WT_STAT_CONN_SET(session, txn_checkpoint_scrub_target, 0);

	if (F_ISSET(txn, WT_TXN_RUNNING)) {
		/*
		 * Clear the dhandle so the visibility check doesn't get
		 * confused about the snap min. Don't bother restoring the
		 * handle since it doesn't make sense to carry a handle across
		 * a checkpoint.
		 */
		session->dhandle = NULL;
		WT_TRET(__wt_txn_rollback(session, NULL));
	}

	/*
	 * Tell logging that we have finished a database checkpoint.  Do not
	 * write a log record if the database was idle.
	 */
	if (full && logging) {
		if (ret == 0 &&
		    F_ISSET(((WT_CURSOR_BTREE *)
		    session->meta_cursor)->btree, WT_BTREE_SKIP_CKPT))
			idle = true;
		WT_TRET(__wt_txn_checkpoint_log(session, full,
		    (ret == 0 && !idle) ?
		    WT_TXN_LOG_CKPT_STOP : WT_TXN_LOG_CKPT_CLEANUP, NULL));
	}

	for (i = 0; i < session->ckpt_handle_next; ++i) {
		if (session->ckpt_handle[i] == NULL)
			continue;
		/*
		 * If the operation failed, mark all trees dirty so they are
		 * included if a future checkpoint can succeed.
		 */
		if (failed)
			WT_WITH_DHANDLE(session, session->ckpt_handle[i],
			    __checkpoint_fail_reset(session));
		WT_WITH_DHANDLE(session, session->ckpt_handle[i],
		    WT_TRET(__wt_session_release_dhandle(session)));
	}

	__wt_free(session, session->ckpt_handle);
	session->ckpt_handle_allocated = session->ckpt_handle_next = 0;

	session->isolation = txn->isolation = saved_isolation;
	return (ret);
}

/*
 * __txn_checkpoint_wrapper --
 *	Checkpoint wrapper.
 */
static int
__txn_checkpoint_wrapper(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DECL_RET;
	WT_TXN_GLOBAL *txn_global;

	txn_global = &S2C(session)->txn_global;

	WT_STAT_CONN_SET(session, txn_checkpoint_running, 1);
	txn_global->checkpoint_running = true;

	ret = __txn_checkpoint(session, cfg);

	WT_STAT_CONN_SET(session, txn_checkpoint_running, 0);
	txn_global->checkpoint_running = false;

	return (ret);
}

/*
 * __wt_txn_checkpoint --
 *	Checkpoint a database or a list of objects in the database.
 */
int
__wt_txn_checkpoint(WT_SESSION_IMPL *session, const char *cfg[], bool waiting)
{
	WT_DECL_RET;
	uint32_t orig_flags;

	/*
	 * Reset open cursors.  Do this explicitly, even though it will happen
	 * implicitly in the call to begin_transaction for the checkpoint, the
	 * checkpoint code will acquire the schema lock before we do that, and
	 * some implementation of WT_CURSOR::reset might need the schema lock.
	 */
	WT_RET(__wt_session_reset_cursors(session, false));

	/* Ensure the metadata table is open before taking any locks. */
	WT_RET(__wt_metadata_cursor(session, NULL));

	/*
	 * Don't highjack the session checkpoint thread for eviction.
	 *
	 * Application threads are not generally available for potentially slow
	 * operations, but checkpoint does enough I/O it may be called upon to
	 * perform slow operations for the block manager.
	 *
	 * Application checkpoints wait until the checkpoint lock is available,
	 * compaction checkpoints don't.
	 *
	 * Checkpoints should always use a separate session for lookaside
	 * updates, otherwise those updates are pinned until the checkpoint
	 * commits.  Also, there are unfortunate interactions between the
	 * special rules for lookaside eviction and the special handling of the
	 * checkpoint transaction.
	 */
#undef WT_CHECKPOINT_SESSION_FLAGS
#define	WT_CHECKPOINT_SESSION_FLAGS \
	(WT_SESSION_CAN_WAIT | WT_SESSION_IGNORE_CACHE_SIZE)
#undef WT_CHECKPOINT_SESSION_FLAGS_OFF
#define	WT_CHECKPOINT_SESSION_FLAGS_OFF \
	(WT_SESSION_LOOKASIDE_CURSOR)
	orig_flags = F_MASK(session,
	    WT_CHECKPOINT_SESSION_FLAGS | WT_CHECKPOINT_SESSION_FLAGS_OFF);
	F_SET(session, WT_CHECKPOINT_SESSION_FLAGS);
	F_CLR(session, WT_CHECKPOINT_SESSION_FLAGS_OFF);

	/*
	 * Only one checkpoint can be active at a time, and checkpoints must run
	 * in the same order as they update the metadata. It's probably a bad
	 * idea to run checkpoints out of multiple threads, but as compaction
	 * calls checkpoint directly, it can be tough to avoid. Serialize here
	 * to ensure we don't get into trouble.
	 */
	if (waiting)
		WT_WITH_CHECKPOINT_LOCK(session,
		    ret = __txn_checkpoint_wrapper(session, cfg));
	else
		WT_WITH_CHECKPOINT_LOCK_NOWAIT(session, ret,
		    ret = __txn_checkpoint_wrapper(session, cfg));

	F_CLR(session, WT_CHECKPOINT_SESSION_FLAGS);
	F_SET(session, orig_flags);

	return (ret);
}

/*
 * __drop --
 *	Drop all checkpoints with a specific name.
 */
static void
__drop(WT_CKPT *ckptbase, const char *name, size_t len)
{
	WT_CKPT *ckpt;

	/*
	 * If we're dropping internal checkpoints, match to the '.' separating
	 * the checkpoint name from the generational number, and take all that
	 * we can find.  Applications aren't allowed to use any variant of this
	 * name, so the test is still pretty simple, if the leading bytes match,
	 * it's one we want to drop.
	 */
	if (strncmp(WT_CHECKPOINT, name, len) == 0) {
		WT_CKPT_FOREACH(ckptbase, ckpt)
			if (WT_PREFIX_MATCH(ckpt->name, WT_CHECKPOINT))
				F_SET(ckpt, WT_CKPT_DELETE);
	} else
		WT_CKPT_FOREACH(ckptbase, ckpt)
			if (WT_STRING_MATCH(ckpt->name, name, len))
				F_SET(ckpt, WT_CKPT_DELETE);
}

/*
 * __drop_from --
 *	Drop all checkpoints after, and including, the named checkpoint.
 */
static void
__drop_from(WT_CKPT *ckptbase, const char *name, size_t len)
{
	WT_CKPT *ckpt;
	bool matched;

	/*
	 * There's a special case -- if the name is "all", then we delete all
	 * of the checkpoints.
	 */
	if (WT_STRING_MATCH("all", name, len)) {
		WT_CKPT_FOREACH(ckptbase, ckpt)
			F_SET(ckpt, WT_CKPT_DELETE);
		return;
	}

	/*
	 * We use the first checkpoint we can find, that is, if there are two
	 * checkpoints with the same name in the list, we'll delete from the
	 * first match to the end.
	 */
	matched = false;
	WT_CKPT_FOREACH(ckptbase, ckpt) {
		if (!matched && !WT_STRING_MATCH(ckpt->name, name, len))
			continue;

		matched = true;
		F_SET(ckpt, WT_CKPT_DELETE);
	}
}

/*
 * __drop_to --
 *	Drop all checkpoints before, and including, the named checkpoint.
 */
static void
__drop_to(WT_CKPT *ckptbase, const char *name, size_t len)
{
	WT_CKPT *ckpt, *mark;

	/*
	 * We use the last checkpoint we can find, that is, if there are two
	 * checkpoints with the same name in the list, we'll delete from the
	 * beginning to the second match, not the first.
	 */
	mark = NULL;
	WT_CKPT_FOREACH(ckptbase, ckpt)
		if (WT_STRING_MATCH(ckpt->name, name, len))
			mark = ckpt;

	if (mark == NULL)
		return;

	WT_CKPT_FOREACH(ckptbase, ckpt) {
		F_SET(ckpt, WT_CKPT_DELETE);

		if (ckpt == mark)
			break;
	}
}

/*
 * __checkpoint_lock_dirty_tree --
 *	Decide whether the tree needs to be included in the checkpoint and if
 *	so, acquire the necessary locks.
 */
static int
__checkpoint_lock_dirty_tree(WT_SESSION_IMPL *session,
    bool is_checkpoint, bool force, bool need_tracking, const char *cfg[])
{
	WT_BTREE *btree;
	WT_CKPT *ckpt, *ckptbase;
	WT_CONFIG dropconf;
	WT_CONFIG_ITEM cval, k, v;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	const char *name;
	char *name_alloc;
	bool hot_backup_locked;

	btree = S2BT(session);
	conn = S2C(session);
	ckpt = ckptbase = NULL;
	dhandle = session->dhandle;
	hot_backup_locked = false;
	name_alloc = NULL;

	/* Only referenced in diagnostic builds. */
	WT_UNUSED(is_checkpoint);

	/*
	 * Only referenced in diagnostic builds and gcc 5.1 isn't satisfied
	 * with wrapping the entire assert condition in the unused macro.
	 */
	WT_UNUSED(need_tracking);

	/*
	 * Most callers need meta tracking to be on here, otherwise it is
	 * possible for this checkpoint to cleanup handles that are still in
	 * use. The exceptions are:
	 *  - Checkpointing the metadata handle itself.
	 *  - On connection close when we know there can't be any races.
	 */
	WT_ASSERT(session, !need_tracking ||
	    WT_IS_METADATA(dhandle) || WT_META_TRACKING(session));

	/* Get the list of checkpoints for this file. */
	WT_RET(__wt_meta_ckptlist_get(session, dhandle->name, &ckptbase));

	/* This may be a named checkpoint, check the configuration. */
	cval.len = 0;
	if (cfg != NULL)
		WT_ERR(__wt_config_gets(session, cfg, "name", &cval));
	if (cval.len == 0)
		name = WT_CHECKPOINT;
	else {
		WT_ERR(__checkpoint_name_ok(session, cval.str, cval.len));
		WT_ERR(__wt_strndup(session, cval.str, cval.len, &name_alloc));
		name = name_alloc;
	}

	/* We may be dropping specific checkpoints, check the configuration. */
	if (cfg != NULL) {
		cval.len = 0;
		WT_ERR(__wt_config_gets(session, cfg, "drop", &cval));
		if (cval.len != 0) {
			__wt_config_subinit(session, &dropconf, &cval);
			while ((ret =
			    __wt_config_next(&dropconf, &k, &v)) == 0) {
				/* Disallow unsafe checkpoint names. */
				if (v.len == 0)
					WT_ERR(__checkpoint_name_ok(
					    session, k.str, k.len));
				else
					WT_ERR(__checkpoint_name_ok(
					    session, v.str, v.len));

				if (v.len == 0)
					__drop(ckptbase, k.str, k.len);
				else if (WT_STRING_MATCH("from", k.str, k.len))
					__drop_from(ckptbase, v.str, v.len);
				else if (WT_STRING_MATCH("to", k.str, k.len))
					__drop_to(ckptbase, v.str, v.len);
				else
					WT_ERR_MSG(session, EINVAL,
					    "unexpected value for checkpoint "
					    "key: %.*s",
					    (int)k.len, k.str);
			}
			WT_ERR_NOTFOUND_OK(ret);
		}
	}

	/* Drop checkpoints with the same name as the one we're taking. */
	__drop(ckptbase, name, strlen(name));

	/* Add a new checkpoint entry at the end of the list. */
	WT_CKPT_FOREACH(ckptbase, ckpt)
		;
	WT_ERR(__wt_strdup(session, name, &ckpt->name));
	/*
	 * We are now done with the local use of the name.  Free the local
	 * allocation, if needed.
	 */
	__wt_free(session, name_alloc);
	F_SET(ckpt, WT_CKPT_ADD);

	/*
	 * We can't delete checkpoints if a backup cursor is open.  WiredTiger
	 * checkpoints are uniquely named and it's OK to have multiple of them
	 * in the system: clear the delete flag for them, and otherwise fail.
	 * Hold the lock until we're done (blocking hot backups from starting),
	 * we don't want to race with a future hot backup.
	 */
	__wt_readlock(session, &conn->hot_backup_lock);
	hot_backup_locked = true;
	if (conn->hot_backup)
		WT_CKPT_FOREACH(ckptbase, ckpt) {
			if (!F_ISSET(ckpt, WT_CKPT_DELETE))
				continue;
			if (WT_PREFIX_MATCH(ckpt->name, WT_CHECKPOINT)) {
				F_CLR(ckpt, WT_CKPT_DELETE);
				continue;
			}
			WT_ERR_MSG(session, EBUSY,
			    "checkpoint %s blocked by hot backup: it would "
			    "delete an existing checkpoint, and checkpoints "
			    "cannot be deleted during a hot backup",
			    ckpt->name);
		}

	/*
	 * Mark old checkpoints that are being deleted and figure out which
	 * trees we can skip in this checkpoint.
	 */
	WT_ERR(__checkpoint_mark_skip(session, ckptbase, force));
	if (F_ISSET(btree, WT_BTREE_SKIP_CKPT))
		goto err;

	/*
	 * Lock the checkpoints that will be deleted.
	 *
	 * Checkpoints are only locked when tracking is enabled, which covers
	 * checkpoint and drop operations, but not close.  The reasoning is
	 * there should be no access to a checkpoint during close, because any
	 * thread accessing a checkpoint will also have the current file handle
	 * open.
	 */
	if (WT_META_TRACKING(session))
		WT_CKPT_FOREACH(ckptbase, ckpt) {
			if (!F_ISSET(ckpt, WT_CKPT_DELETE))
				continue;

			/*
			 * We can't delete checkpoints referenced by a cursor.
			 * WiredTiger checkpoints are uniquely named and it's
			 * OK to have multiple in the system: clear the delete
			 * flag for them, and otherwise fail.
			 */
			ret = __wt_session_lock_checkpoint(session, ckpt->name);
			if (ret == 0)
				continue;
			if (ret == EBUSY &&
			    WT_PREFIX_MATCH(ckpt->name, WT_CHECKPOINT)) {
				F_CLR(ckpt, WT_CKPT_DELETE);
				continue;
			}
			WT_ERR_MSG(session, ret,
			    "checkpoints cannot be dropped when in-use");
		}

	/*
	 * There are special tree: those being bulk-loaded, salvaged, upgraded
	 * or verified during the checkpoint. They should never be part of a
	 * checkpoint: we will fail to lock them because the operations have
	 * exclusive access to the handles. Named checkpoints will fail in that
	 * case, ordinary checkpoints will skip files that cannot be opened
	 * normally.
	 */
	WT_ASSERT(session,
	    !is_checkpoint || !F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS));

	__wt_readunlock(session, &conn->hot_backup_lock);

	WT_ASSERT(session, btree->ckpt == NULL &&
	    !F_ISSET(btree, WT_BTREE_SKIP_CKPT));
	btree->ckpt = ckptbase;

	return (0);

err:	if (hot_backup_locked)
		__wt_readunlock(session, &conn->hot_backup_lock);

	__wt_meta_ckptlist_free(session, &ckptbase);
	__wt_free(session, name_alloc);

	return (ret);
}

/*
 * __checkpoint_mark_skip --
 *	Figure out whether the checkpoint can be skipped for a tree.
 */
static int
__checkpoint_mark_skip(
    WT_SESSION_IMPL *session, WT_CKPT *ckptbase, bool force)
{
	WT_BTREE *btree;
	WT_CKPT *ckpt;
	int deleted;
	const char *name;

	btree = S2BT(session);

	/*
	 * Check for clean objects not requiring a checkpoint.
	 *
	 * If we're closing a handle, and the object is clean, we can skip the
	 * checkpoint, whatever checkpoints we have are sufficient.  (We might
	 * not have any checkpoints if the object was never modified, and that's
	 * OK: the object creation code doesn't mark the tree modified so we can
	 * skip newly created trees here.)
	 *
	 * If the application repeatedly checkpoints an object (imagine hourly
	 * checkpoints using the same explicit or internal name), there's no
	 * reason to repeat the checkpoint for clean objects.  The test is if
	 * the only checkpoint we're deleting is the last one in the list and
	 * it has the same name as the checkpoint we're about to take, skip the
	 * work.  (We can't skip checkpoints that delete more than the last
	 * checkpoint because deleting those checkpoints might free up space in
	 * the file.)  This means an application toggling between two (or more)
	 * checkpoint names will repeatedly take empty checkpoints, but that's
	 * not likely enough to make detection worthwhile.
	 *
	 * Checkpoint read-only objects otherwise: the application must be able
	 * to open the checkpoint in a cursor after taking any checkpoint, which
	 * means it must exist.
	 */
	F_CLR(btree, WT_BTREE_SKIP_CKPT);
	if (!btree->modified && !force) {
		deleted = 0;
		WT_CKPT_FOREACH(ckptbase, ckpt)
			if (F_ISSET(ckpt, WT_CKPT_DELETE))
				++deleted;

		/*
		 * Complicated test: if the tree is clean and last two
		 * checkpoints have the same name (correcting for internal
		 * checkpoint names with their generational suffix numbers), we
		 * can skip the checkpoint, there's nothing to do.  The
		 * exception is if we're deleting two or more checkpoints: then
		 * we may save space.
		 */
		name = (ckpt - 1)->name;
		if (ckpt > ckptbase + 1 && deleted < 2 &&
		    (strcmp(name, (ckpt - 2)->name) == 0 ||
		    (WT_PREFIX_MATCH(name, WT_CHECKPOINT) &&
		    WT_PREFIX_MATCH((ckpt - 2)->name, WT_CHECKPOINT)))) {
			F_SET(btree, WT_BTREE_SKIP_CKPT);
			return (0);
		}
	}

	return (0);
}

/*
 * __checkpoint_tree --
 *	Checkpoint a single tree.
 *	Assumes all necessary locks have been acquired by the caller.
 */
static int
__checkpoint_tree(
    WT_SESSION_IMPL *session, bool is_checkpoint, const char *cfg[])
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_CKPT *ckpt, *ckptbase;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	WT_LSN ckptlsn;
	bool fake_ckpt, resolve_bm;

	WT_UNUSED(cfg);

	btree = S2BT(session);
	bm = btree->bm;
	ckptbase = btree->ckpt;
	conn = S2C(session);
	dhandle = session->dhandle;
	fake_ckpt = resolve_bm = false;

	/*
	 * Set the checkpoint LSN to the maximum LSN so that if logging is
	 * disabled, recovery will never roll old changes forward over the
	 * non-logged changes in this checkpoint.  If logging is enabled, a
	 * real checkpoint LSN will be assigned for this checkpoint and
	 * overwrite this.
	 */
	WT_MAX_LSN(&ckptlsn);

	/*
	 * If an object has never been used (in other words, if it could become
	 * a bulk-loaded file), then we must fake the checkpoint.  This is good
	 * because we don't write physical checkpoint blocks for just-created
	 * files, but it's not just a good idea.  The reason is because deleting
	 * a physical checkpoint requires writing the file, and fake checkpoints
	 * can't write the file.  If you (1) create a physical checkpoint for an
	 * empty file which writes blocks, (2) start bulk-loading records into
	 * the file, (3) during the bulk-load perform another checkpoint with
	 * the same name; in order to keep from having two checkpoints with the
	 * same name you would have to use the bulk-load's fake checkpoint to
	 * delete a physical checkpoint, and that will end in tears.
	 */
	if (is_checkpoint)
		if (btree->original) {
			fake_ckpt = true;
			goto fake;
		}

	/*
	 * Mark the root page dirty to ensure something gets written. (If the
	 * tree is modified, we must write the root page anyway, this doesn't
	 * add additional writes to the process.  If the tree is not modified,
	 * we have to dirty the root page to ensure something gets written.)
	 * This is really about paranoia: if the tree modification value gets
	 * out of sync with the set of dirty pages (modify is set, but there
	 * are no dirty pages), we perform a checkpoint without any writes, no
	 * checkpoint is created, and then things get bad.
	 * While marking the root page as dirty, we do not want to dirty the
	 * btree because we are marking the btree as clean just after this call.
	 * Also, marking the btree dirty at this stage will unnecessarily mark
	 * the connection as dirty causing checkpoint-skip code to fail.
	 */
	WT_ERR(__wt_page_modify_init(session, btree->root.page));
	__wt_page_only_modify_set(session, btree->root.page);

	/*
	 * Clear the tree's modified flag; any changes before we clear the flag
	 * are guaranteed to be part of this checkpoint (unless reconciliation
	 * skips updates for transactional reasons), and changes subsequent to
	 * the checkpoint start, which might not be included, will re-set the
	 * modified flag.  The "unless reconciliation skips updates" problem is
	 * handled in the reconciliation code: if reconciliation skips updates,
	 * it sets the modified flag itself.
	 */
	btree->modified = false;
	WT_FULL_BARRIER();

	/* Tell logging that a file checkpoint is starting. */
	if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		WT_ERR(__wt_txn_checkpoint_log(
		    session, false, WT_TXN_LOG_CKPT_START, &ckptlsn));

	/* Tell the block manager that a file checkpoint is starting. */
	WT_ERR(bm->checkpoint_start(bm, session));
	resolve_bm = true;

	/* Flush the file from the cache, creating the checkpoint. */
	if (is_checkpoint)
		WT_ERR(__wt_cache_op(session, WT_SYNC_CHECKPOINT));
	else
		WT_ERR(__wt_cache_op(session, WT_SYNC_CLOSE));

	/*
	 * All blocks being written have been written; set the object's write
	 * generation.
	 */
	WT_CKPT_FOREACH(ckptbase, ckpt)
		if (F_ISSET(ckpt, WT_CKPT_ADD))
			ckpt->write_gen = btree->write_gen;

fake:	/*
	 * If we're faking a checkpoint and logging is enabled, recovery should
	 * roll forward any changes made between now and the next checkpoint,
	 * so set the checkpoint LSN to the beginning of time.
	 */
	if (fake_ckpt && FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		WT_INIT_LSN(&ckptlsn);

	/*
	 * Update the object's metadata.
	 *
	 * If the object is the metadata, the call to __wt_meta_ckptlist_set
	 * will update the turtle file and swap the new one into place.  We
	 * need to make sure the metadata is on disk before the turtle file is
	 * updated.
	 *
	 * If we are doing a checkpoint in a file without a transaction (e.g.,
	 * closing a dirty tree before an exclusive operation like verify),
	 * the metadata update will be auto-committed.  In that case, we need to
	 * sync the file here or we could roll forward the metadata in
	 * recovery and open a checkpoint that isn't yet durable.
	 */
	if (WT_IS_METADATA(dhandle) ||
	    !F_ISSET(&session->txn, WT_TXN_RUNNING))
		WT_ERR(__wt_checkpoint_sync(session, NULL));

	WT_ERR(__wt_meta_ckptlist_set(
	    session, dhandle->name, ckptbase, &ckptlsn));

	/*
	 * If we wrote a checkpoint (rather than faking one), we have to resolve
	 * it. Normally, tracking is enabled and resolution deferred until
	 * transaction end. The exception is if the handle is being discarded,
	 * in which case the handle will be gone by the time we try to apply or
	 * unroll the meta tracking event.
	 */
	if (!fake_ckpt) {
		resolve_bm = false;
		if (WT_META_TRACKING(session) && is_checkpoint)
			WT_ERR(__wt_meta_track_checkpoint(session));
		else
			WT_ERR(bm->checkpoint_resolve(bm, session, false));
	}

	/* Tell logging that the checkpoint is complete. */
	if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		WT_ERR(__wt_txn_checkpoint_log(
		    session, false, WT_TXN_LOG_CKPT_STOP, NULL));

err:	/* Resolved the checkpoint for the block manager in the error path. */
	if (resolve_bm)
		WT_TRET(bm->checkpoint_resolve(bm, session, ret != 0));

	/*
	 * If the checkpoint didn't complete successfully, make sure the
	 * tree is marked dirty.
	 */
	if (ret != 0) {
		btree->modified = true;
		conn->modified = true;
	}

	__wt_meta_ckptlist_free(session, &btree->ckpt);

	return (ret);
}

/*
 * __checkpoint_presync --
 *	Visit all handles after the checkpoint writes are complete and before
 *	syncing.  At this point, all trees should be completely open for
 *	business.
 */
static int
__checkpoint_presync(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BTREE *btree;

	WT_UNUSED(cfg);

	btree = S2BT(session);
	WT_ASSERT(session,
	    btree->checkpoint_gen == __wt_gen(session, WT_GEN_CHECKPOINT));
	btree->evict_walk_period = btree->evict_walk_saved;
	return (0);
}

/*
 * __checkpoint_tree_helper --
 *	Checkpoint a tree (suitable for use in *_apply functions).
 */
static int
__checkpoint_tree_helper(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_TXN *txn;
	bool with_timestamp;

	btree = S2BT(session);
	txn = &session->txn;

	/* Are we using a read timestamp for this checkpoint transaction? */
	with_timestamp = F_ISSET(txn, WT_TXN_HAS_TS_READ);

	/*
	 * For tables with immediate durability (indicated by having logging
	 * enabled), ignore any read timestamp configured for the checkpoint.
	 */
	if (__wt_btree_immediately_durable(session))
		F_CLR(txn, WT_TXN_HAS_TS_READ);

	ret = __checkpoint_tree(session, true, cfg);

	/* Restore the use of the timestamp for other tables. */
	if (with_timestamp)
		F_SET(txn, WT_TXN_HAS_TS_READ);

	/*
	 * Whatever happened, we aren't visiting this tree again in this
	 * checkpoint.  Don't keep updates pinned any longer.
	 */
	__checkpoint_update_generation(session);

	/*
	 * In case this tree was being skipped by the eviction server
	 * during the checkpoint, restore the previous state.
	 */
	btree->evict_walk_period = btree->evict_walk_saved;

	/*
	 * Wake the eviction server, in case application threads have
	 * stalled while the eviction server decided it couldn't make
	 * progress.  Without this, application threads will be stalled
	 * until the eviction server next wakes.
	 */
	__wt_evict_server_wake(session);

	return (ret);
}

/*
 * __wt_checkpoint --
 *	Checkpoint a file.
 */
int
__wt_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	bool force;

	/* Should not be called with a checkpoint handle. */
	WT_ASSERT(session, session->dhandle->checkpoint == NULL);

	/* We must hold the metadata lock if checkpointing the metadata. */
	WT_ASSERT(session, !WT_IS_METADATA(session->dhandle) ||
	    F_ISSET(session, WT_SESSION_LOCKED_METADATA));

	WT_RET(__wt_config_gets_def(session, cfg, "force", 0, &cval));
	force = cval.val != 0;
	WT_SAVE_DHANDLE(session, ret = __checkpoint_lock_dirty_tree(
	    session, true, force, true, cfg));
	WT_RET(ret);
	if (F_ISSET(S2BT(session), WT_BTREE_SKIP_CKPT))
		return (0);
	return (__checkpoint_tree(session, true, cfg));
}

/*
 * __wt_checkpoint_sync --
 *	Sync a file that has been checkpointed, and wait for the result.
 */
int
__wt_checkpoint_sync(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BM *bm;

	WT_UNUSED(cfg);

	bm = S2BT(session)->bm;

	/* Should not be called with a checkpoint handle. */
	WT_ASSERT(session, session->dhandle->checkpoint == NULL);

	/* Unnecessary if checkpoint_sync has been configured "off". */
	if (!F_ISSET(S2C(session), WT_CONN_CKPT_SYNC))
		return (0);

	return (bm->sync(bm, session, true));
}

/*
 * __wt_checkpoint_close --
 *	Checkpoint a single file as part of closing the handle.
 */
int
__wt_checkpoint_close(WT_SESSION_IMPL *session, bool final)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	bool bulk, need_tracking;

	btree = S2BT(session);
	bulk = F_ISSET(btree, WT_BTREE_BULK);

	/*
	 * We've done the final checkpoint before the final close, subsequent
	 * writes to normal objects are wasted effort. Discard the objects to
	 * validate exit accounting.
	 */
	if (final && !WT_IS_METADATA(session->dhandle))
		return (__wt_cache_op(session, WT_SYNC_DISCARD));

	/*
	 * If closing an unmodified file, check that no update is required
	 * for active readers.
	 */
	if (!btree->modified && !bulk) {
		WT_RET(__wt_txn_update_oldest(
		    session, WT_TXN_OLDEST_STRICT | WT_TXN_OLDEST_WAIT));
		return (__wt_txn_visible_all(session, btree->rec_max_txn,
		    WT_TIMESTAMP_NULL(&btree->rec_max_timestamp)) ?
		    __wt_cache_op(session, WT_SYNC_DISCARD) : EBUSY);
	}

	/*
	 * Don't flush data from trees when there is a stable timestamp set:
	 * that can lead to files that are inconsistent on disk after a crash.
	 */
	if (btree->modified && !bulk &&
	    S2C(session)->txn_global.has_stable_timestamp &&
	    !__wt_btree_immediately_durable(session))
		return (EBUSY);

	/*
	 * Turn on metadata tracking if:
	 * - The session is not already doing metadata tracking.
	 * - The file was not bulk loaded.
	 * - The close is not during connection close.
	 */
	need_tracking = !WT_META_TRACKING(session) && !bulk && !final;

	if (need_tracking)
		WT_RET(__wt_meta_track_on(session));

	WT_SAVE_DHANDLE(session, ret = __checkpoint_lock_dirty_tree(
	    session, false, false, need_tracking, NULL));
	WT_ASSERT(session, ret == 0);
	if (ret == 0 && !F_ISSET(btree, WT_BTREE_SKIP_CKPT))
		ret = __checkpoint_tree(session, false, NULL);

	if (need_tracking)
		WT_TRET(__wt_meta_track_off(session, true, ret != 0));

	return (ret);
}

/*
 * __checkpoint_timing_stress --
 *	Optionally add a 10 second delay to a checkpoint to simulate a long
 *	running checkpoint for debug purposes. The reason for this option is
 *	finding	operations that can block while waiting for a checkpoint to
 *	complete.
 */
static void
__checkpoint_timing_stress(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*
	 * We only want to sleep if the flag is set and the checkpoint comes
	 * from the API, so check if the session used is either of the two
	 * sessions set aside for internal checkpoints.
	 */
	if (conn->ckpt_session != session &&
	    conn->meta_ckpt_session != session &&
	    FLD_ISSET(conn->timing_stress_flags,
	    WT_TIMING_STRESS_CHECKPOINT_SLOW))
		__wt_sleep(10, 0);
}
