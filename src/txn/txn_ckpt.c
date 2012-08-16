/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_txn_checkpoint --
 *	Checkpoint a database or a list of objects in the database.
 */
int
__wt_txn_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_BTREE *btree, *saved_btree;
	WT_CONFIG targetconf;
	WT_CONFIG_ITEM cval, k, v;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	void *saved_meta_next;
	int ckpt_closed, target_list, tracking;

	conn = S2C(session);
	target_list = tracking = 0;
	txn = &session->txn;
	txn_global = &conn->txn_global;

	/* Only one checkpoint can be active at a time. */
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	/*
	 * Checkpoints require a snapshot to write a transactionally consistent
	 * snapshot of the data.
	 *
	 * We can't use an application's transaction: if it has uncommitted
	 * changes, they will be written in the checkpoint and may appear after
	 * a crash.
	 *
	 * Don't start a real transaction with its own ID: although there
	 * should be no conflicts when updating the metadata, we don't want to
	 * take the risk of aborting after doing all of the I/O.  In addition,
	 * the tracking of non-transactional readers is complicated by having a
	 * full transaction: there may be non-transactional cursors open in
	 * this session that hold hazard references, so we may already have an
	 * ID in the global state table.
	 */
	if (F_ISSET(txn, TXN_RUNNING))
		WT_RET_MSG(session, EINVAL,
		    "Checkpoint not permitted in a transaction");

	WT_RET(__wt_txn_get_snapshot(session, WT_TXN_NONE));
	txn->isolation = TXN_ISO_SNAPSHOT;

	/* Prevent eviction from evicting anything newer than this. */
	txn_global->ckpt_txnid = txn->snap_min;

	WT_ERR(__wt_meta_track_on(session));
	tracking = 1;

	/* Step through the list of targets and checkpoint each one. */
	WT_ERR(__wt_config_gets(session, cfg, "target", &cval));
	WT_ERR(__wt_config_subinit(session, &targetconf, &cval));
	while ((ret = __wt_config_next(&targetconf, &k, &v)) == 0) {
		if (!target_list) {
			WT_ERR(__wt_scr_alloc(session, 512, &tmp));
			target_list = 1;
		}

		if (v.len != 0)
			WT_ERR_MSG(session, EINVAL,
			    "invalid checkpoint target \"%s\": URIs may "
			    "require quoting",
			    (const char *)tmp->data);

		WT_ERR(__wt_buf_fmt(session, tmp, "%.*s", (int)k.len, k.str));
		if ((ret = __wt_schema_worker(
		    session, tmp->data, __wt_checkpoint, cfg, 0)) != 0)
			WT_ERR_MSG(session, ret, "%s", (const char *)tmp->data);
	}
	WT_ERR_NOTFOUND_OK(ret);

	if (!target_list) {
		/*
		 * Possible checkpoint name.  If checkpoints are named or we're
		 * dropping checkpoints, checkpoint both open and closed files;
		 * else, we only checkpoint open files.
		 *
		 * XXX
		 * We don't optimize unnamed checkpoints of a list of targets,
		 * we open the targets and checkpoint them even if they are
		 * quiescent and don't need a checkpoint, believing applications
		 * unlikely to checkpoint a list of closed targets.
		 */
		cval.len = 0;
		ckpt_closed = 0;
		WT_ERR(__wt_config_gets(session, cfg, "name", &cval));
		if (cval.len != 0)
			ckpt_closed = 1;
		WT_ERR(__wt_config_gets(session, cfg, "drop", &cval));
		if (cval.len != 0)
			ckpt_closed = 1;
		WT_ERR(ckpt_closed ?
		    __wt_meta_btree_apply(session, __wt_checkpoint, cfg, 0) :
		    __wt_conn_btree_apply(session, __wt_checkpoint, cfg));
	}

	/* Checkpoint the metadata file. */
	TAILQ_FOREACH(btree, &conn->btqh, q)
		if (strcmp(btree->name, WT_METADATA_URI) == 0)
			break;
	if (btree == NULL)
		WT_ERR_MSG(session, EINVAL,
		    "checkpoint unable to find open meta-data handle");

	/*
	 * Disable metadata tracking during the metadata checkpoint.
	 *
	 * We don't lock old checkpoints in the metadata file: there is no way
	 * to open one.  We are holding other handle locks, it is not safe to
	 * lock conn->spinlock.
	 */
	saved_meta_next = session->meta_track_next;
	session->meta_track_next = NULL;
	saved_btree = session->btree;
	session->btree = btree;
	ret = __wt_checkpoint(session, cfg);
	session->btree = saved_btree;
	session->meta_track_next = saved_meta_next;
	WT_ERR(ret);

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
	if (tracking)
		WT_TRET(__wt_meta_track_off(session, ret != 0));

	txn_global->ckpt_txnid = WT_TXN_NONE;
	__wt_txn_release_snapshot(session);
	txn->isolation = session->isolation;
	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __ckpt_name_ok --
 *	Complain if our reserved checkpoint name is used.
 */
static int
__ckpt_name_ok(WT_SESSION_IMPL *session, const char *name, size_t len)
{
	/*
	 * The internal checkpoint name is special, applications aren't allowed
	 * to use it.  Be aggressive and disallow any matching prefix, it makes
	 * things easier when checking in other places.
	 */
	if (len < strlen(WT_CHECKPOINT))
		return (0);
	if (strncmp(name, WT_CHECKPOINT, strlen(WT_CHECKPOINT)) != 0)
		return (0);

	WT_RET_MSG(session, EINVAL,
	    "the checkpoint name \"%s\" is reserved", WT_CHECKPOINT);
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
			if (strncmp(ckpt->name,
			    WT_CHECKPOINT, strlen(WT_CHECKPOINT)) == 0)
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
 * __wt_checkpoint --
 *	Checkpoint a tree.
 */
int
__wt_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BTREE *btree;
	WT_CKPT *ckpt, *ckptbase, *new;
	WT_CONFIG dropconf;
	WT_CONFIG_ITEM cval, k, v;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	const char *name;
	char *name_alloc;
	int deleted, force, is_checkpoint;

	conn = S2C(session);
	btree = session->btree;
	force = 0;
	ckpt = ckptbase = NULL;
	name_alloc = NULL;

	/*
	 * We're called in two ways: either because a handle is closing or
	 * session.checkpoint was called, figure it out.
	 */
	is_checkpoint = cfg == NULL ? 0 : 1;

	/*
	 * Checkpoint handles are read-only by definition and don't participate
	 * in checkpoints.   Closing one discards its blocks, otherwise there's
	 * no work to do.
	 */
	if (btree->checkpoint != NULL)
		return (is_checkpoint ? 0 :
		    __wt_bt_cache_flush(
		    session, NULL, WT_SYNC_DISCARD_NOWRITE));

	/*
	 * If closing a file that's never been modified, discard its blocks.
	 * If checkpoint of a file that's never been modified, we may still
	 * have to checkpoint it, we'll test again once we understand the
	 * nature of the checkpoint.
	 */
	if (btree->modified == 0 && !is_checkpoint)
		return (__wt_bt_cache_flush(
		    session, NULL, WT_SYNC_DISCARD_NOWRITE));

	/*
	 * Get the list of checkpoints for this file.  If there's no reference
	 * to the file in the metadata (the file is dead), then discard it from
	 * the cache without bothering to write any dirty pages.
	 */
	if ((ret = __wt_meta_ckptlist_get(
	    session, btree->name, &ckptbase)) == WT_NOTFOUND)
		return (__wt_bt_cache_flush(
		    session, NULL, WT_SYNC_DISCARD_NOWRITE));
	WT_ERR(ret);

	/* This may be a named checkpoint, check the configuration. */
	cval.len = 0;
	if (cfg != NULL)
		WT_ERR(__wt_config_gets(session, cfg, "name", &cval));
	if (cval.len == 0)
		name = WT_CHECKPOINT;
	else {
		WT_ERR(__ckpt_name_ok(session, cval.str, cval.len));

		/*
		 * Dropping and naming checkpoints require a checkpoint even
		 * if the tree is clean.
		 */
		force = 1;
		WT_ERR(__wt_strndup(session, cval.str, cval.len, &name_alloc));
		name = name_alloc;
	}

	/*
	 * We may be dropping specific checkpoints, check the configuration.
	 * If we're dropping checkpoints, set force, we have to create the
	 * checkpoint even if the tree is clean.
	 */
	if (cfg != NULL) {
		cval.len = 0;
		WT_ERR(__wt_config_gets(session, cfg, "drop", &cval));
		if (cval.len != 0) {
			WT_ERR(__wt_config_subinit(session, &dropconf, &cval));
			while ((ret =
			    __wt_config_next(&dropconf, &k, &v)) == 0) {
				/* Disallow the reserved checkpoint name. */
				if (v.len == 0)
					WT_ERR(__ckpt_name_ok(
					    session, k.str, k.len));
				else
					WT_ERR(__ckpt_name_ok(
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

				/* Dropping requires a checkpoint. */
				force = 1;
			}
			WT_ERR_NOTFOUND_OK(ret);
		}
	}

	/*
	 * Ignore read-only objects if we don't have to take a checkpoint.  If
	 * force is set because the checkpoint is named or there's an explicit
	 * command to drop a checkpoint, we'll modify the file's root page and
	 * ensure a checkpoint happens, but otherwise, the object isn't dirty
	 * and the existing checkpoints are sufficient.
	 */
	if (btree->modified == 0 && !force)
		goto skip;

	/* Drop checkpoints with the same name as the one we're taking. */
	__drop(ckptbase, name, strlen(name));

	/*
	 * Another check for read-only objects not requiring a checkpoint.  If
	 * the application repeatedly checkpoints the same name (imagine taking
	 * an hourly checkpoint using the same name), there's no reason to
	 * repeat the checkpoint for read-only objects.  If the only checkpoint
	 * we're deleting is the last one in the list, and it has the same name
	 * as the checkpoint we're about to take, skip the work.
	 */
	if (btree->modified == 0) {
		deleted = 0;
		WT_CKPT_FOREACH(ckptbase, ckpt)
			if (F_ISSET(ckpt, WT_CKPT_DELETE))
				++deleted;
		if (deleted == 1 &&
		    F_ISSET(ckpt - 1, WT_CKPT_DELETE) &&
		    strcmp(name, (ckpt - 1)->name) == 0)
			goto skip;
	}

	/* Add a new checkpoint entry at the end of the list. */
	WT_CKPT_FOREACH(ckptbase, ckpt)
		;
	WT_ERR(__wt_strdup(session, name, &ckpt->name));
	F_SET(ckpt, WT_CKPT_ADD);
	new = ckpt;

	/*
	 * Lock the checkpoints that will be deleted.
	 *
	 * Checkpoints are only locked when tracking is enabled, which covers
	 * sync and drop operations, but not close.  The reasoning is that
	 * there should be no access to a checkpoint during close, because any
	 * thread accessing a checkpoint will also have the current file handle
	 * open.
	 */
	if (WT_META_TRACKING(session))
		WT_CKPT_FOREACH(ckptbase, ckpt) {
			if (!F_ISSET(ckpt, WT_CKPT_DELETE))
				continue;

			/*
			 * We can't drop/update checkpoints if a backup cursor
			 * is open.  WiredTiger checkpoints are uniquely named
			 * and it's OK to have multiple in the system: clear the
			 * delete flag, and otherwise fail.
			 */
			if (conn->ckpt_backup) {
				if (strncmp(ckpt->name,
				    WT_CHECKPOINT,
				    strlen(WT_CHECKPOINT)) == 0) {
					F_CLR(ckpt, WT_CKPT_DELETE);
					continue;
				}
				WT_ERR_MSG(session, EBUSY,
				    "checkpoints cannot be dropped when "
				    "backup cursors are open");
			}

			/*
			 * We can't drop/update checkpoints if referenced by a
			 * cursor.  WiredTiger checkpoints are uniquely named
			 * and it's OK to have multiple in the system: clear the
			 * delete flag, and otherwise fail.
			 */
			ret =
			    __wt_session_lock_checkpoint(session, ckpt->name);
			if (ret == 0)
				continue;
			if (ret == EBUSY && strncmp(ckpt->name,
			    WT_CHECKPOINT, strlen(WT_CHECKPOINT)) == 0) {
				F_CLR(ckpt, WT_CKPT_DELETE);
				continue;
			}
			WT_ERR_MSG(session, ret,
			    "checkpoints cannot be dropped when in-use");
		}

	/*
	 * If we're forcing a checkpoint, notify the cache in case the file
	 * has no dirty pages.
	 */
	if (force)
		WT_ERR(__wt_bt_cache_force_write(session));

	/*
	 * Clear the tree's modified value and ensure that write happens before
	 * we reconcile/write any pages.   Any changes before we clear the flag
	 * are guaranteed to be part of this checkpoint (unless reconciliation
	 * skips updates for transactional reasons), and changes subsequent to
	 * the checkpoint start, which might not be included, will re-set the
	 * modified flag.  The "unless reconciliation skips updates" problem is
	 * handled in the reconciliation code: if reconciliation skips updates,
	 * it sets the modified flag itself.
	 */
	WT_PUBLISH(btree->modified, 0);


	/* If closing a handle, include everything in the checkpoint. */
	if (!is_checkpoint)
		session->txn.isolation = TXN_ISO_READ_UNCOMMITTED;

	/* Flush the file from the cache, creating the checkpoint. */
	WT_ERR(__wt_bt_cache_flush(session,
	    ckptbase, is_checkpoint ? WT_SYNC : WT_SYNC_DISCARD));

	/* If there was a checkpoint, update the metadata and resolve it. */
	if (new->raw.data == NULL) {
		/*
		 * Our knowledge of whether or not the file has dirty pages
		 * isn't perfect, we only know if the file was ever modified.
		 * If we didn't really need a checkpoint, it's not a problem
		 * if one wasn't created.
		 */
		if (force)
			WT_ERR_MSG(session, EINVAL,
			    "cache flush failed to create a checkpoint");
	} else {
		WT_ERR(__wt_meta_ckptlist_set(session, btree->name, ckptbase));
		/*
		 * If tracking is enabled, defer making pages available until
		 * the end of the transaction.  The exception is if the handle
		 * is being discarded: in that case, it will be gone by the
		 * time we try to apply or unroll the meta tracking event.
		 */
		if (WT_META_TRACKING(session) && is_checkpoint)
			WT_ERR(__wt_meta_track_checkpoint(session));
		else
			WT_ERR(__wt_bm_checkpoint_resolve(session));
	}

err:
skip:	__wt_meta_ckptlist_free(session, ckptbase);
	__wt_free(session, name_alloc);

	if (!is_checkpoint)
		session->txn.isolation = session->isolation;

	return (ret);
}
