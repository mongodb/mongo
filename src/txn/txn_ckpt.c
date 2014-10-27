/*-
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
 * __checkpoint_apply --
 *	Apply an operation to all files involved in a checkpoint.
 */
static int
__checkpoint_apply(WT_SESSION_IMPL *session, const char *cfg[],
	int (*op)(WT_SESSION_IMPL *, const char *[]), int *fullp)
{
	WT_CONFIG targetconf;
	WT_CONFIG_ITEM cval, k, v;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	int ckpt_closed, named, target_list;

	target_list = 0;

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
			target_list = 1;
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
		    __wt_conn_btree_apply(session, 0, op, cfg));
	}

	if (fullp != NULL)
		*fullp = !target_list;

err:	__wt_scr_free(&tmp);
	return (ret);
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
	WT_DATA_HANDLE *saved_dhandle;
	WT_DECL_RET;
	const char *name;

	WT_UNUSED(cfg);

	/* Should not be called with anything other than a file object. */
	WT_ASSERT(session, session->dhandle->checkpoint == NULL);
	WT_ASSERT(session,
	    memcmp(session->dhandle->name, "file:", strlen("file:")) == 0);

	/* Make sure there is space for the next entry. */
	WT_RET(__wt_realloc_def(session, &session->ckpt_handle_allocated,
	    session->ckpt_handle_next + 1, &session->ckpt_handle));

	/* Not strictly necessary, but cleaner to clear the current handle. */
	name = session->dhandle->name;
	saved_dhandle = session->dhandle;
	session->dhandle = NULL;

	/* Ignore busy files, we'll deal with them in the checkpoint. */
	switch (ret = __wt_session_get_btree(session, name, NULL, NULL, 0)) {
	case 0:
		session->ckpt_handle[
		    session->ckpt_handle_next++] = session->dhandle;
		break;
	case EBUSY:
		ret = 0;
		break;
	default:
		break;
	}

	session->dhandle = saved_dhandle;
	return (ret);
}

/*
 * __checkpoint_write_leaves --
 *	Write any dirty leaf pages for all checkpoint handles.
 */
static int
__checkpoint_write_leaves(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	u_int i;

	i = 0;

	/* Should not be called with any handle reference. */
	WT_ASSERT(session, session->dhandle == NULL);

	/*
	 * Get a list of handles we want to flush; this may pull closed objects
	 * into the session cache, but we're going to do that eventually anyway.
	 */
	WT_WITH_SCHEMA_LOCK(session,
	    ret = __checkpoint_apply(session, cfg, __wt_checkpoint_list, NULL));
	WT_ERR(ret);

	/*
	 * Walk the list, flushing the leaf pages from each file, then releasing
	 * the file.  Note that we increment inside the loop to simplify error
	 * handling.
	 */
	while (i < session->ckpt_handle_next) {
		dhandle = session->ckpt_handle[i++];
		WT_WITH_DHANDLE(session, dhandle,
		    ret = __wt_cache_op(session, NULL, WT_SYNC_WRITE_LEAVES));
		WT_WITH_DHANDLE(session, dhandle,
		    WT_TRET(__wt_session_release_btree(session)));
		WT_ERR(ret);
	}

err:	while (i < session->ckpt_handle_next) {
		dhandle = session->ckpt_handle[i++];
		WT_WITH_DHANDLE(session, dhandle,
		    WT_TRET(__wt_session_release_btree(session)));
	}
	__wt_free(session, session->ckpt_handle);
	session->ckpt_handle_allocated = session->ckpt_handle_next = 0;
	return (ret);
}

/*
 * __wt_txn_checkpoint --
 *	Checkpoint a database or a list of objects in the database.
 */
int
__wt_txn_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	WT_TXN *txn;
	WT_TXN_ISOLATION saved_isolation;
	int full, logging, tracking;
	const char *txn_cfg[] =
	    { WT_CONFIG_BASE(session, session_begin_transaction),
	      "isolation=snapshot", NULL };
	void *saved_meta_next;

	conn = S2C(session);
	saved_isolation = session->isolation;
	txn = &session->txn;
	full = logging = tracking = 0;

	/*
	 * Do a pass over the configuration arguments and figure out what kind
	 * kind of checkpoint this is.
	 */
	WT_RET(__checkpoint_apply(session, cfg, NULL, &full));

	/*
	 * Update the global oldest ID so we do all possible cleanup.
	 *
	 * This is particularly important for compact, so that all dirty pages
	 * can be fully written.
	 */
	__wt_txn_update_oldest(session);

	/* Flush data-sources before we start the checkpoint. */
	WT_ERR(__checkpoint_data_source(session, cfg));

	/* Flush dirty leaf pages before we start the checkpoint. */
	session->isolation = txn->isolation = TXN_ISO_READ_COMMITTED;
	WT_ERR(__checkpoint_write_leaves(session, cfg));

	/* Acquire the schema lock. */
	F_SET(session, WT_SESSION_SCHEMA_LOCKED);
	__wt_spin_lock(session, &conn->schema_lock);

	WT_ERR(__wt_meta_track_on(session));
	tracking = 1;

	/* Tell logging that we are about to start a database checkpoint. */
	if (conn->logging && full)
		WT_ERR(__wt_txn_checkpoint_log(
		    session, full, WT_TXN_LOG_CKPT_PREPARE, NULL));

	/*
	 * Start a snapshot transaction for the checkpoint.
	 *
	 * Note: we don't go through the public API calls because they have
	 * side effects on cursors, which applications can hold open across
	 * calls to checkpoint.
	 */
	WT_ERR(__wt_txn_begin(session, txn_cfg));

	/* Tell logging that we have started a database checkpoint. */
	if (conn->logging && full) {
		WT_ERR(__wt_txn_checkpoint_log(
		    session, full, WT_TXN_LOG_CKPT_START, NULL));
		logging = 1;
	}

	WT_ERR(__checkpoint_apply(session, cfg, __wt_checkpoint, NULL));

	/* Commit the transaction before syncing the file(s). */
	WT_ERR(__wt_txn_commit(session, NULL));

	/*
	 * Checkpoints have to hit disk (it would be reasonable to configure for
	 * lazy checkpoints, but we don't support them yet).
	 */
	if (F_ISSET(conn, WT_CONN_CKPT_SYNC))
		WT_ERR(__checkpoint_apply(
		    session, cfg, __wt_checkpoint_sync, NULL));

	/* Checkpoint the metadata file. */
	SLIST_FOREACH(dhandle, &conn->dhlh, l) {
		if (WT_IS_METADATA(dhandle) ||
		    !WT_PREFIX_MATCH(dhandle->name, "file:"))
			break;
	}
	if (dhandle == NULL)
		WT_ERR_MSG(session, EINVAL,
		    "checkpoint unable to find open meta-data handle");

	/*
	 * Disable metadata tracking during the metadata checkpoint.
	 *
	 * We don't lock old checkpoints in the metadata file: there is no way
	 * to open one.  We are holding other handle locks, it is not safe to
	 * lock conn->spinlock.
	 */
	session->isolation = txn->isolation = TXN_ISO_READ_UNCOMMITTED;
	saved_meta_next = session->meta_track_next;
	session->meta_track_next = NULL;
	WT_WITH_DHANDLE(session, dhandle, ret = __wt_checkpoint(session, cfg));
	session->meta_track_next = saved_meta_next;

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
	session->isolation = txn->isolation = TXN_ISO_READ_UNCOMMITTED;
	if (tracking)
		WT_TRET(__wt_meta_track_off(session, ret != 0));

	if (F_ISSET(txn, TXN_RUNNING))
		WT_TRET(__wt_txn_rollback(session, NULL));

	/* Tell logging that we have finished a database checkpoint. */
	if (logging)
		WT_TRET(__wt_txn_checkpoint_log(session, full,
		    (ret == 0) ? WT_TXN_LOG_CKPT_STOP : WT_TXN_LOG_CKPT_FAIL,
		    NULL));

	if (F_ISSET(session, WT_SESSION_SCHEMA_LOCKED)) {
		F_CLR(session, WT_SESSION_SCHEMA_LOCKED);
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
	int matched;

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
	matched = 0;
	WT_CKPT_FOREACH(ckptbase, ckpt) {
		if (!matched && !WT_STRING_MATCH(ckpt->name, name, len))
			continue;

		matched = 1;
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
    WT_SESSION_IMPL *session, const char *cfg[], int is_checkpoint)
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
	const char *name;
	int deleted, force, hot_backup_locked, track_ckpt, was_modified;
	char *name_alloc;

	btree = S2BT(session);
	bm = btree->bm;
	conn = S2C(session);
	ckpt = ckptbase = NULL;
	INIT_LSN(&ckptlsn);
	dhandle = session->dhandle;
	name_alloc = NULL;
	hot_backup_locked = 0;
	name_alloc = NULL;
	track_ckpt = 1;
	was_modified = btree->modified;

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
	force = 0;
	if (!btree->modified && cfg != NULL) {
		ret = __wt_config_gets(session, cfg, "force", &cval);
		if (ret != 0 && ret != WT_NOTFOUND)
			WT_ERR(ret);
		if (ret == 0 && cval.val != 0)
			force = 1;
	}
	if (!btree->modified && !force) {
		if (!is_checkpoint)
			goto done;

		deleted = 0;
		WT_CKPT_FOREACH(ckptbase, ckpt)
			if (F_ISSET(ckpt, WT_CKPT_DELETE))
				++deleted;
		/*
		 * Complicated test: if we only deleted a single checkpoint, and
		 * it was the last checkpoint in the object, and it has the same
		 * name as the checkpoint we're taking (correcting for internal
		 * checkpoint names with their generational suffix numbers), we
		 * can skip the checkpoint, there's nothing to do.
		 */
		if (deleted == 1 &&
		    F_ISSET(ckpt - 1, WT_CKPT_DELETE) &&
		    (strcmp(name, (ckpt - 1)->name) == 0 ||
		    (WT_PREFIX_MATCH(name, WT_CHECKPOINT) &&
		    WT_PREFIX_MATCH((ckpt - 1)->name, WT_CHECKPOINT))))
			goto done;
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
	__wt_spin_lock(session, &conn->hot_backup_lock);
	hot_backup_locked = 1;
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
		switch (F_ISSET(btree, WT_BTREE_SPECIAL_FLAGS)) {
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
			track_ckpt = 0;
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
			track_ckpt = 0;
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
	if (conn->logging)
		WT_ERR(__wt_txn_checkpoint_log(
		    session, 0, WT_TXN_LOG_CKPT_START, &ckptlsn));

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

fake:	/* Update the object's metadata. */
	WT_ERR(__wt_meta_ckptlist_set(
	    session, dhandle->name, ckptbase, &ckptlsn));

	/*
	 * If we wrote a checkpoint (rather than faking one), pages may be
	 * available for re-use.  If tracking enabled, defer making pages
	 * available until transaction end.  The exception is if the handle
	 * is being discarded, in which case the handle will be gone by the
	 * time we try to apply or unroll the meta tracking event.
	 */
	if (track_ckpt) {
		if (WT_META_TRACKING(session) && is_checkpoint)
			WT_ERR(__wt_meta_track_checkpoint(session));
		else
			WT_ERR(bm->checkpoint_resolve(bm, session));
	}

	/* Tell logging that the checkpoint is complete. */
	if (conn->logging)
		WT_ERR(__wt_txn_checkpoint_log(
		    session, 0, WT_TXN_LOG_CKPT_STOP, NULL));

done: err:
	/*
	 * If the checkpoint didn't complete successfully, make sure the
	 * tree is marked dirty.
	 */
	if (ret != 0 && !btree->modified && was_modified)
		btree->modified = 1;

	if (hot_backup_locked)
		__wt_spin_unlock(session, &conn->hot_backup_lock);

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
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	return (__checkpoint_worker(session, cfg, 1));
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

	return (bm->sync(bm, session, 0));
}

/*
 * __wt_checkpoint_close --
 *	Checkpoint a single file as part of closing the handle.
 */
int
__wt_checkpoint_close(WT_SESSION_IMPL *session, int force)
{
	/* If closing an unmodified file, simply discard its blocks. */
	if (!S2BT(session)->modified || force)
		return (__wt_cache_op(session, NULL,
		    force ? WT_SYNC_DISCARD_FORCE : WT_SYNC_DISCARD));

	/*
	 * Else, checkpoint the file and optionally flush the writes (the
	 * checkpoint call will discard the blocks, there's no additional
	 * step needed).
	 */
	WT_RET(__checkpoint_worker(session, NULL, 0));
	if (F_ISSET(S2C(session), WT_CONN_CKPT_SYNC))
		WT_RET(__wt_checkpoint_sync(session, NULL));

	return (0);
}
