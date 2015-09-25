/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_checkpoint_name_ok --
 *	Complain if the checkpoint name isn't acceptable.
 */
int
__wt_checkpoint_name_ok(WT_SESSION_IMPL *session, const char *name, size_t len)
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
	 * confirm the metadata file contains no non-file objects.
	 */
	if (uri == NULL) {
		WT_ERR(__wt_metadata_cursor(session, NULL, &cursor));
		while ((ret = cursor->next(cursor)) == 0) {
			WT_ERR(cursor->get_key(cursor, &uri));
			if (!WT_PREFIX_MATCH(uri, "colgroup:") &&
			    !WT_PREFIX_MATCH(uri, "file:") &&
			    !WT_PREFIX_MATCH(uri, "index:") &&
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

err:	if (cursor != NULL)
		WT_TRET(cursor->close(cursor));
	return (ret);
}

/*
 * __checkpoint_apply_all --
 *	Apply an operation to all files involved in a checkpoint.
 */
static int
__checkpoint_apply_all(WT_SESSION_IMPL *session, const char *cfg[],
	int (*op)(WT_SESSION_IMPL *, const char *[]), bool *fullp)
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
		WT_RET(__wt_checkpoint_name_ok(session, cval.str, cval.len));

	/* Step through the targets and optionally operate on each one. */
	WT_ERR(__wt_config_gets(session, cfg, "target", &cval));
	WT_ERR(__wt_config_subinit(session, &targetconf, &cval));
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
		    __wt_meta_btree_apply(session, op, cfg) :
		    __wt_conn_btree_apply(session, false, NULL, op, cfg));
	}

	if (fullp != NULL)
		*fullp = !target_list;

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
		if (session->ckpt_handle[i].dhandle != NULL)
			WT_WITH_DHANDLE(session,
			    session->ckpt_handle[i].dhandle,
			    ret = (*op)(session, cfg));
		else
			WT_WITH_HANDLE_LIST_LOCK(session,
			    ret = __wt_conn_btree_apply_single(session,
			    session->ckpt_handle[i].name, NULL, op, cfg));
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
	WT_NAMED_DATA_SOURCE *ndsrc;
	WT_DATA_SOURCE *dsrc;

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
 * __wt_checkpoint_list --
 *	Get a list of handles to flush.
 */
int
__wt_checkpoint_list(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DECL_RET;
	const char *name;

	WT_UNUSED(cfg);

	/* Should not be called with anything other than a file object. */
	WT_ASSERT(session, session->dhandle->checkpoint == NULL);
	WT_ASSERT(session, WT_PREFIX_MATCH(session->dhandle->name, "file:"));

	/* Skip files that are never involved in a checkpoint. */
	if (F_ISSET(S2BT(session), WT_BTREE_NO_CHECKPOINT))
		return (0);

	/* Make sure there is space for the next entry. */
	WT_RET(__wt_realloc_def(session, &session->ckpt_handle_allocated,
	    session->ckpt_handle_next + 1, &session->ckpt_handle));

	/* Not strictly necessary, but cleaner to clear the current handle. */
	name = session->dhandle->name;
	session->dhandle = NULL;

	/* Record busy file names, we'll deal with them in the checkpoint. */
	if ((ret = __wt_session_get_btree(session, name, NULL, NULL, 0)) == 0)
		session->ckpt_handle[session->ckpt_handle_next++].dhandle =
		    session->dhandle;
	else if (ret == EBUSY)
		ret = __wt_strdup(session, name,
		    &session->ckpt_handle[session->ckpt_handle_next++].name);

	return (ret);
}

/*
 * __checkpoint_write_leaves --
 *	Write any dirty leaf pages for all checkpoint handles.
 */
static int
__checkpoint_write_leaves(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_UNUSED(cfg);

	return (__wt_cache_op(session, NULL, WT_SYNC_WRITE_LEAVES));
}

/*
 * __checkpoint_stats --
 *	Update checkpoint timer stats.
 */
static void
__checkpoint_stats(
    WT_SESSION_IMPL *session, struct timespec *start, struct timespec *stop)
{
	WT_CONNECTION_IMPL *conn;
	uint64_t msec;

	conn = S2C(session);

	/*
	 * Get time diff in microseconds.
	 */
	msec = WT_TIMEDIFF(*stop, *start) / WT_MILLION;

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
static int
__checkpoint_verbose_track(WT_SESSION_IMPL *session,
    const char *msg, struct timespec *start)
{
#ifdef HAVE_VERBOSE
	struct timespec stop;
	uint64_t msec;

	if (!WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT))
		return (0);

	WT_RET(__wt_epoch(session, &stop));

	/*
	 * Get time diff in microseconds.
	 */
	msec = WT_TIMEDIFF(stop, *start) / WT_MILLION;
	WT_RET(__wt_verbose(session,
	    WT_VERB_CHECKPOINT, "time: %" PRIu64 " us, gen: %" PRIu64
	    ": Full database checkpoint %s",
	    msec, S2C(session)->txn_global.checkpoint_gen, msg));

	/* Update the timestamp so we are reporting intervals. */
	memcpy(start, &stop, sizeof(*start));
#else
	WT_UNUSED(session);
	WT_UNUSED(msg);
	WT_UNUSED(start);
#endif
	return (0);
}

/*
 * __wt_txn_checkpoint --
 *	Checkpoint a database or a list of objects in the database.
 */
int
__wt_txn_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
{
	struct timespec start, stop, verb_timer;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_ISOLATION saved_isolation;
	WT_TXN_STATE *txn_state;
	void *saved_meta_next;
	u_int i;
	bool full, idle, logging, tracking;
	const char *txn_cfg[] = { WT_CONFIG_BASE(session,
	    WT_SESSION_begin_transaction), "isolation=snapshot", NULL };

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;
	txn_state = WT_SESSION_TXN_STATE(session);
	saved_isolation = session->isolation;
	full = idle = logging = tracking = false;

	/* Ensure the metadata table is open before taking any locks. */
	WT_RET(__wt_metadata_open(session));

	/*
	 * Do a pass over the configuration arguments and figure out what kind
	 * kind of checkpoint this is.
	 */
	WT_RET(__checkpoint_apply_all(session, cfg, NULL, &full));

	/* Configure logging only if doing a full checkpoint. */
	logging = FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED);

	/*
	 * Get a list of handles we want to flush; this may pull closed objects
	 * into the session cache, but we're going to do that eventually anyway.
	 */
	WT_WITH_SCHEMA_LOCK(session,
	    WT_WITH_TABLE_LOCK(session,
		WT_WITH_HANDLE_LIST_LOCK(session,
		    ret = __checkpoint_apply_all(
		    session, cfg, __wt_checkpoint_list, NULL))));
	WT_ERR(ret);

	/*
	 * Update the global oldest ID so we do all possible cleanup.
	 *
	 * This is particularly important for compact, so that all dirty pages
	 * can be fully written.
	 */
	__wt_txn_update_oldest(session, true);

	/* Flush data-sources before we start the checkpoint. */
	WT_ERR(__checkpoint_data_source(session, cfg));

	WT_ERR(__wt_epoch(session, &verb_timer));
	WT_ERR(__checkpoint_verbose_track(session,
	    "starting write leaves", &verb_timer));

	/* Flush dirty leaf pages before we start the checkpoint. */
	session->isolation = txn->isolation = WT_ISO_READ_COMMITTED;
	WT_ERR(__checkpoint_apply(session, cfg, __checkpoint_write_leaves));

	/*
	 * The underlying flush routine scheduled an asynchronous flush
	 * after writing the leaf pages, but in order to minimize I/O
	 * while holding the schema lock, do a flush and wait for the
	 * completion. Do it after flushing the pages to give the
	 * asynchronous flush as much time as possible before we wait.
	 */
	if (F_ISSET(conn, WT_CONN_CKPT_SYNC))
		WT_ERR(__checkpoint_apply(session, cfg, __wt_checkpoint_sync));

	/* Acquire the schema lock. */
	F_SET(session, WT_SESSION_LOCKED_SCHEMA);
	__wt_spin_lock(session, &conn->schema_lock);

	WT_ERR(__wt_meta_track_on(session));
	tracking = true;

	/* Tell logging that we are about to start a database checkpoint. */
	if (full && logging)
		WT_ERR(__wt_txn_checkpoint_log(
		    session, full, WT_TXN_LOG_CKPT_PREPARE, NULL));

	WT_ERR(__checkpoint_verbose_track(session,
	    "starting transaction", &verb_timer));

	if (full)
		WT_ERR(__wt_epoch(session, &start));

	/*
	 * Bump the global checkpoint generation, used to figure out whether
	 * checkpoint has visited a tree.  There is no need for this to be
	 * atomic: it is only written while holding the checkpoint lock.
	 *
	 * We do need to update it before clearing the checkpoint's entry out
	 * of the transaction table, or a thread evicting in a tree could
	 * ignore the checkpoint's transaction.
	 */
	++txn_global->checkpoint_gen;
	WT_STAT_FAST_CONN_SET(session,
	    txn_checkpoint_generation, txn_global->checkpoint_gen);

	/*
	 * Start a snapshot transaction for the checkpoint.
	 *
	 * Note: we don't go through the public API calls because they have
	 * side effects on cursors, which applications can hold open across
	 * calls to checkpoint.
	 */
	WT_ERR(__wt_txn_begin(session, txn_cfg));

	/* Ensure a transaction ID is allocated prior to sharing it globally */
	WT_ERR(__wt_txn_id_check(session));

	/*
	 * Save the checkpoint session ID.  We never do checkpoints in the
	 * default session (with id zero).
	 */
	WT_ASSERT(session, session->id != 0 && txn_global->checkpoint_id == 0);
	txn_global->checkpoint_id = session->id;

	txn_global->checkpoint_pinned =
	    WT_MIN(txn_state->id, txn_state->snap_min);

	/*
	 * We're about to clear the checkpoint transaction from the global
	 * state table so the oldest ID can move forward.  Make sure everything
	 * we've done above is scheduled.
	 */
	WT_FULL_BARRIER();

	/*
	 * Sanity check that the oldest ID hasn't moved on before we have
	 * cleared our entry.
	 */
	WT_ASSERT(session,
	    WT_TXNID_LE(txn_global->oldest_id, txn_state->id) &&
	    WT_TXNID_LE(txn_global->oldest_id, txn_state->snap_min));

	/*
	 * Clear our entry from the global transaction session table. Any
	 * operation that needs to know about the ID for this checkpoint will
	 * consider the checkpoint ID in the global structure. Most operations
	 * can safely ignore the checkpoint ID (see the visible all check for
	 * details).
	 */
	txn_state->id = txn_state->snap_min = WT_TXN_NONE;

	/* Tell logging that we have started a database checkpoint. */
	if (full && logging)
		WT_ERR(__wt_txn_checkpoint_log(
		    session, full, WT_TXN_LOG_CKPT_START, NULL));

	WT_ERR(__checkpoint_apply(session, cfg, __wt_checkpoint));

	/*
	 * Clear the dhandle so the visibility check doesn't get confused about
	 * the snap min. Don't bother restoring the handle since it doesn't
	 * make sense to carry a handle across a checkpoint.
	 */
	session->dhandle = NULL;

	/* Release the snapshot so we aren't pinning pages in cache. */
	__wt_txn_release_snapshot(session);

	WT_ERR(__checkpoint_verbose_track(session,
	    "committing transaction", &verb_timer));

	/*
	 * Checkpoints have to hit disk (it would be reasonable to configure for
	 * lazy checkpoints, but we don't support them yet).
	 */
	if (F_ISSET(conn, WT_CONN_CKPT_SYNC))
		WT_ERR(__checkpoint_apply(session, cfg, __wt_checkpoint_sync));

	WT_ERR(__checkpoint_verbose_track(session,
	    "sync completed", &verb_timer));

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
	 */
	if (full || !logging) {
		session->isolation = txn->isolation = WT_ISO_READ_UNCOMMITTED;
		/* Disable metadata tracking during the metadata checkpoint. */
		saved_meta_next = session->meta_track_next;
		session->meta_track_next = NULL;
		WT_WITH_DHANDLE(session,
		    session->meta_dhandle, ret = __wt_checkpoint(session, cfg));
		session->meta_track_next = saved_meta_next;
		WT_ERR(ret);

		WT_ERR(__checkpoint_verbose_track(session,
		    "metadata sync completed", &verb_timer));
	} else
		WT_WITH_DHANDLE(session, session->meta_dhandle,
		    ret = __wt_txn_checkpoint_log(
		    session, false, WT_TXN_LOG_CKPT_SYNC, NULL));

	if (full) {
		WT_ERR(__wt_epoch(session, &stop));
		__checkpoint_stats(session, &start, &stop);
	}

err:	/*
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
	session->isolation = txn->isolation = WT_ISO_READ_UNCOMMITTED;
	if (tracking)
		WT_TRET(__wt_meta_track_off(session, false, ret != 0));

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
		    F_ISSET((WT_BTREE *)session->meta_dhandle->handle,
		    WT_BTREE_SKIP_CKPT))
			idle = true;
		WT_TRET(__wt_txn_checkpoint_log(session, full,
		    (ret == 0 && !idle) ?
		    WT_TXN_LOG_CKPT_STOP : WT_TXN_LOG_CKPT_CLEANUP, NULL));
	}

	for (i = 0; i < session->ckpt_handle_next; ++i) {
		if (session->ckpt_handle[i].dhandle == NULL) {
			__wt_free(session, session->ckpt_handle[i].name);
			continue;
		}
		WT_WITH_DHANDLE(session, session->ckpt_handle[i].dhandle,
		    WT_TRET(__wt_session_release_btree(session)));
	}

	__wt_free(session, session->ckpt_handle);
	session->ckpt_handle_allocated = session->ckpt_handle_next = 0;

	if (F_ISSET(session, WT_SESSION_LOCKED_SCHEMA)) {
		F_CLR(session, WT_SESSION_LOCKED_SCHEMA);
		__wt_spin_unlock(session, &conn->schema_lock);
	}

	session->isolation = txn->isolation = saved_isolation;
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
 * __checkpoint_worker --
 *	Checkpoint a tree.
 */
static int
__checkpoint_worker(
    WT_SESSION_IMPL *session, const char *cfg[], bool is_checkpoint)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_CKPT *ckpt, *ckptbase;
	WT_CONFIG dropconf;
	WT_CONFIG_ITEM cval, k, v;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	WT_LSN ckptlsn;
	int deleted, was_modified;
	bool fake_ckpt, force, hot_backup_locked;
	const char *name;
	char *name_alloc;

	btree = S2BT(session);
	bm = btree->bm;
	conn = S2C(session);
	ckpt = ckptbase = NULL;
	dhandle = session->dhandle;
	was_modified = btree->modified;
	fake_ckpt = hot_backup_locked = false;
	name_alloc = NULL;

	/*
	 * Set the checkpoint LSN to the maximum LSN so that if logging is
	 * disabled, recovery will never roll old changes forward over the
	 * non-logged changes in this checkpoint.  If logging is enabled, a
	 * real checkpoint LSN will be assigned later for this checkpoint and
	 * overwrite this.
	 */
	WT_MAX_LSN(&ckptlsn);

	/* Get the list of checkpoints for this file. */
	WT_RET(__wt_meta_ckptlist_get(session, dhandle->name, &ckptbase));

	/* This may be a named checkpoint, check the configuration. */
	cval.len = 0;
	if (cfg != NULL)
		WT_ERR(__wt_config_gets(session, cfg, "name", &cval));
	if (cval.len == 0)
		name = WT_CHECKPOINT;
	else {
		WT_ERR(__wt_checkpoint_name_ok(session, cval.str, cval.len));
		WT_ERR(__wt_strndup(session, cval.str, cval.len, &name_alloc));
		name = name_alloc;
	}

	/* We may be dropping specific checkpoints, check the configuration. */
	if (cfg != NULL) {
		cval.len = 0;
		WT_ERR(__wt_config_gets(session, cfg, "drop", &cval));
		if (cval.len != 0) {
			WT_ERR(__wt_config_subinit(session, &dropconf, &cval));
			while ((ret =
			    __wt_config_next(&dropconf, &k, &v)) == 0) {
				/* Disallow unsafe checkpoint names. */
				if (v.len == 0)
					WT_ERR(__wt_checkpoint_name_ok(
					    session, k.str, k.len));
				else
					WT_ERR(__wt_checkpoint_name_ok(
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
	force = false;
	F_CLR(btree, WT_BTREE_SKIP_CKPT);
	if (!btree->modified && cfg != NULL) {
		ret = __wt_config_gets(session, cfg, "force", &cval);
		if (ret != 0 && ret != WT_NOTFOUND)
			WT_ERR(ret);
		if (ret == 0 && cval.val != 0)
			force = true;
	}
	if (!btree->modified && !force) {
		if (!is_checkpoint)
			goto nockpt;

		deleted = 0;
		WT_CKPT_FOREACH(ckptbase, ckpt)
			if (F_ISSET(ckpt, WT_CKPT_DELETE))
				++deleted;
		/*
		 * Complicated test: if the last checkpoint in the object has
		 * the same name as the checkpoint we're taking (correcting for
		 * internal checkpoint names with their generational suffix
		 * numbers), we can skip the checkpoint, there's nothing to do.
		 * The exception is if we're deleting two or more checkpoints:
		 * then we may save space.
		 */
		if (ckpt > ckptbase &&
		    (strcmp(name, (ckpt - 1)->name) == 0 ||
		    (WT_PREFIX_MATCH(name, WT_CHECKPOINT) &&
		    WT_PREFIX_MATCH((ckpt - 1)->name, WT_CHECKPOINT))) &&
		    deleted < 2) {
nockpt:			F_SET(btree, WT_BTREE_SKIP_CKPT);
			WT_PUBLISH(btree->checkpoint_gen,
			    S2C(session)->txn_global.checkpoint_gen);
			WT_STAT_FAST_DATA_SET(session,
			    btree_checkpoint_generation,
			    btree->checkpoint_gen);
			goto done;
		}
	}

	/* Add a new checkpoint entry at the end of the list. */
	WT_CKPT_FOREACH(ckptbase, ckpt)
		;
	WT_ERR(__wt_strdup(session, name, &ckpt->name));
	F_SET(ckpt, WT_CKPT_ADD);

	/*
	 * We can't delete checkpoints if a backup cursor is open.  WiredTiger
	 * checkpoints are uniquely named and it's OK to have multiple of them
	 * in the system: clear the delete flag for them, and otherwise fail.
	 * Hold the lock until we're done (blocking hot backups from starting),
	 * we don't want to race with a future hot backup.
	 */
	WT_ERR(__wt_readlock(session, conn->hot_backup_lock));
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
	 * There are special files: those being bulk-loaded, salvaged, upgraded
	 * or verified during the checkpoint.  We have to do something for those
	 * objects because a checkpoint is an external name the application can
	 * reference and the name must exist no matter what's happening during
	 * the checkpoint.  For bulk-loaded files, we could block until the load
	 * completes, checkpoint the partial load, or magic up an empty-file
	 * checkpoint.  The first is too slow, the second is insane, so do the
	 * third.
	 *    Salvage, upgrade and verify don't currently require any work, all
	 * three hold the schema lock, blocking checkpoints. If we ever want to
	 * fix that (and I bet we eventually will, at least for verify), we can
	 * copy the last checkpoint the file has.  That works if we guarantee
	 * salvage, upgrade and verify act on objects with previous checkpoints
	 * (true if handles are closed/re-opened between object creation and a
	 * subsequent salvage, upgrade or verify operation).  Presumably,
	 * salvage and upgrade will discard all previous checkpoints when they
	 * complete, which is fine with us.  This change will require reference
	 * counting checkpoints, and once that's done, we should use checkpoint
	 * copy instead of forcing checkpoints on clean objects to associate
	 * names with checkpoints.
	 */
	if (is_checkpoint)
		switch (F_MASK(btree, WT_BTREE_SPECIAL_FLAGS)) {
		case 0:
			break;
		case WT_BTREE_BULK:
			/*
			 * The only checkpoints a bulk-loaded file should have
			 * are fake ones we created without the underlying block
			 * manager.  I'm leaving this code here because it's a
			 * cheap test and a nasty race.
			 */
			WT_CKPT_FOREACH(ckptbase, ckpt)
				if (!F_ISSET(ckpt, WT_CKPT_ADD | WT_CKPT_FAKE))
					WT_ERR_MSG(session, ret,
					    "block-manager checkpoint found "
					    "for a bulk-loaded file");
			fake_ckpt = true;
			goto fake;
		case WT_BTREE_SALVAGE:
		case WT_BTREE_UPGRADE:
		case WT_BTREE_VERIFY:
			WT_ERR_MSG(session, EINVAL,
			    "checkpoints are blocked during salvage, upgrade "
			    "or verify operations");
		}

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
		if (btree->bulk_load_ok) {
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
	 */
	WT_ERR(__wt_page_modify_init(session, btree->root.page));
	__wt_page_modify_set(session, btree->root.page);

	/*
	 * Clear the tree's modified flag; any changes before we clear the flag
	 * are guaranteed to be part of this checkpoint (unless reconciliation
	 * skips updates for transactional reasons), and changes subsequent to
	 * the checkpoint start, which might not be included, will re-set the
	 * modified flag.  The "unless reconciliation skips updates" problem is
	 * handled in the reconciliation code: if reconciliation skips updates,
	 * it sets the modified flag itself.  Use a full barrier so we get the
	 * store done quickly, this isn't a performance path.
	 */
	btree->modified = 0;
	WT_FULL_BARRIER();

	/* Tell logging that a file checkpoint is starting. */
	if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		WT_ERR(__wt_txn_checkpoint_log(
		    session, false, WT_TXN_LOG_CKPT_START, &ckptlsn));

	/* Flush the file from the cache, creating the checkpoint. */
	if (is_checkpoint)
		WT_ERR(__wt_cache_op(session, ckptbase, WT_SYNC_CHECKPOINT));
	else
		WT_ERR(__wt_cache_op(session, ckptbase, WT_SYNC_CLOSE));

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
	if (F_ISSET(conn, WT_CONN_CKPT_SYNC) &&
	    (WT_IS_METADATA(dhandle) ||
	    !F_ISSET(&session->txn, WT_TXN_RUNNING)))
		WT_ERR(__wt_checkpoint_sync(session, NULL));

	WT_ERR(__wt_meta_ckptlist_set(
	    session, dhandle->name, ckptbase, &ckptlsn));

	/*
	 * If we wrote a checkpoint (rather than faking one), pages may be
	 * available for re-use.  If tracking enabled, defer making pages
	 * available until transaction end.  The exception is if the handle
	 * is being discarded, in which case the handle will be gone by the
	 * time we try to apply or unroll the meta tracking event.
	 */
	if (!fake_ckpt) {
		if (WT_META_TRACKING(session) && is_checkpoint)
			WT_ERR(__wt_meta_track_checkpoint(session));
		else
			WT_ERR(bm->checkpoint_resolve(bm, session));
	}

	/* Tell logging that the checkpoint is complete. */
	if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		WT_ERR(__wt_txn_checkpoint_log(
		    session, false, WT_TXN_LOG_CKPT_STOP, NULL));

done:
err:	/*
	 * If the checkpoint didn't complete successfully, make sure the
	 * tree is marked dirty.
	 */
	if (ret != 0 && !btree->modified && was_modified)
		btree->modified = 1;

	if (hot_backup_locked)
		WT_TRET(__wt_readunlock(session, conn->hot_backup_lock));

	__wt_meta_ckptlist_free(session, ckptbase);
	__wt_free(session, name_alloc);

	return (ret);
}

/*
 * __wt_checkpoint --
 *	Checkpoint a file.
 */
int
__wt_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
{
	/* Should not be called with a checkpoint handle. */
	WT_ASSERT(session, session->dhandle->checkpoint == NULL);

	/* Should be holding the schema lock. */
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_SCHEMA));

	return (__checkpoint_worker(session, cfg, true));
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

	/* Should have an underlying block manager reference. */
	WT_ASSERT(session, bm != NULL);

	return (bm->sync(bm, session, false));
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
	 * If the handle is already dead or the file isn't durable, force the
	 * discard.
	 *
	 * If the file isn't durable, mark the handle dead, there are asserts
	 * later on that only dead handles can have modified pages.
	 */
	if (F_ISSET(btree, WT_BTREE_NO_CHECKPOINT))
		F_SET(session->dhandle, WT_DHANDLE_DEAD);
	if (F_ISSET(session->dhandle, WT_DHANDLE_DEAD))
		return (__wt_cache_op(session, NULL, WT_SYNC_DISCARD));

	/*
	 * If closing an unmodified file, check that no update is required
	 * for active readers.
	 */
	if (!btree->modified && !bulk) {
		__wt_txn_update_oldest(session, true);
		return (__wt_txn_visible_all(session, btree->rec_max_txn) ?
		    __wt_cache_op(session, NULL, WT_SYNC_DISCARD) : EBUSY);
	}

	/*
	 * We should already have the schema lock unless we're finishing a bulk
	 * load -- the only other paths to closing files (sweep and LSM) have
	 * already checked for read-only trees.
	 */
	WT_ASSERT(session,
	    final || bulk || F_ISSET(session, WT_SESSION_LOCKED_SCHEMA));

	/*
	 * Turn on metadata tracking if:
	 * - The session is not already doing metadata tracking.
	 * - The file was bulk loaded.
	 * - The close is not during connection close.
	 */
	need_tracking = !WT_META_TRACKING(session) && !bulk && !final;

	if (need_tracking)
		WT_RET(__wt_meta_track_on(session));

	WT_TRET(__checkpoint_worker(session, NULL, false));

	if (need_tracking)
		WT_RET(__wt_meta_track_off(session, true, ret != 0));

	return (ret);
}
