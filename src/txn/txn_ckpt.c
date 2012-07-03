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
	WT_TXN_GLOBAL *txn_global;
	void *saved_meta_next;
	int target_list, tracking;
	const char *txn_cfg[] = { "isolation=snapshot", NULL };

	conn = S2C(session);
	target_list = tracking = 0;
	txn_global = &conn->txn_global;

	/* Only one checkpoint can be active at a time. */
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	WT_ERR(__wt_txn_begin(session, txn_cfg));

	/* Prevent eviction from evicting anything newer than this. */
	txn_global->ckpt_txnid = session->txn.snap_min;

	WT_ERR(__wt_meta_track_on(session));
	tracking = 1;

	/* Step through the list of targets and checkpoint each one. */
	cval.len = 0;
	WT_ERR(__wt_config_gets(session, cfg, "target", &cval));
	if (cval.len != 0) {
		WT_ERR(__wt_scr_alloc(session, 512, &tmp));
		WT_ERR(__wt_config_subinit(session, &targetconf, &cval));
		while ((ret = __wt_config_next(&targetconf, &k, &v)) == 0) {
			target_list = 1;
			WT_ERR(__wt_buf_fmt(session, tmp, "%.*s",
			    (int)k.len, k.str));

			if (v.len != 0)
				WT_ERR_MSG(session, EINVAL,
				    "invalid checkpoint target \"%s\": "
				    "URIs may require quoting",
				    (const char *)tmp->data);

			ret = __wt_schema_worker(
			    session, tmp->data, __wt_checkpoint, cfg, 0);

			if (ret != 0)
				WT_ERR_MSG(session, ret, "%s",
				    (const char *)tmp->data);
		}
		WT_ERR_NOTFOUND_OK(ret);
	}

	if (!target_list) {
		/*
		 * Possible checkpoint name.  If checkpoints are named, we must
		 * checkpoint both open and closed files; if checkpoints are not
		 * named, we only checkpoint open files.
		 *
		 * XXX
		 * We don't optimize unnamed checkpoints of a list of targets,
		 * we open the targets and checkpoint them even if they are
		 * quiescent and don't need a checkpoint, believing applications
		 * unlikely to checkpoint a list of closed targets.
		 */
		cval.len = 0;
		WT_ERR(__wt_config_gets(session, cfg, "name", &cval));
		WT_ERR(cval.len == 0 ?
		    __wt_conn_btree_apply(session, __wt_checkpoint, cfg) :
		    __wt_meta_btree_apply(session, __wt_checkpoint, cfg, 0));
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
	if (F_ISSET(&session->txn, TXN_RUNNING))
		WT_TRET(__wt_txn_release(session));
	__wt_scr_free(&tmp);
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

	WT_CKPT_FOREACH(ckptbase, ckpt)
		if (strlen(ckpt->name) == len &&
		    strncmp(ckpt->name, name, len) == 0)
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
	if (len == strlen("all") && strncmp(name, "all", len) == 0) {
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
		if (!matched &&
		    (strlen(ckpt->name) != len ||
		    strncmp(ckpt->name, name, len) != 0))
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
		if (strlen(ckpt->name) == len &&
		    strncmp(ckpt->name, name, len) == 0)
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
	WT_CKPT *ckpt, *ckptbase, *deleted;
	WT_CONFIG dropconf;
	WT_CONFIG_ITEM cval, k, v;
	WT_DECL_RET;
	const char *name;
	char *name_alloc;
	int force, tracked;

	btree = session->btree;
	force = tracked = 0;
	ckpt = ckptbase = NULL;
	name_alloc = NULL;

	/*
	 * Get the list of checkpoints for this file.  If there's no reference,
	 * this file is dead.  Discard it from the cache without bothering to
	 * write any dirty pages.
	 */
	if ((ret =
	    __wt_meta_ckptlist_get(session, btree->name, &ckptbase)) != 0) {
		if (ret == WT_NOTFOUND)
			ret = __wt_bt_cache_flush(
			    session, NULL, WT_SYNC_DISCARD_NOWRITE, 0);
		goto err;
	}

	/*
	 * This may be a named checkpoint, check the configuration.  If it's a
	 * named checkpoint, set force, we have to create the checkpoint even if
	 * the tree is clean.
	 */
	cval.len = 0;
	if (cfg != NULL)
		WT_ERR(__wt_config_gets(session, cfg, "name", &cval));
	if (cval.len == 0)
		name = WT_INTERNAL_CHKPT;
	else {
		force = 1;
		WT_ERR(__wt_strndup(session, cval.str, cval.len, &name_alloc));
		name = name_alloc;
	}

	/*
	 * We may be dropping checkpoints, check the configuration.  If we're
	 * dropping checkpoints, set force, we have to create the checkpoint
	 * even if the tree is clean.
	 */
	if (cfg != NULL) {
		cval.len = 0;
		WT_ERR(__wt_config_gets(session, cfg, "drop", &cval));
		if (cval.len != 0) {
			WT_ERR(__wt_config_subinit(session, &dropconf, &cval));
			while ((ret =
			    __wt_config_next(&dropconf, &k, &v)) == 0) {
				force = 1;

				if (v.len == 0)
					__drop(ckptbase, k.str, k.len);
				else if (k.len == strlen("from") &&
				    strncmp(k.str, "from", k.len) == 0)
					__drop_from(ckptbase, v.str, v.len);
				else if (k.len == strlen("to") &&
				    strncmp(k.str, "to", k.len) == 0)
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

	/* Discard checkpoints with the same name as the new checkpoint. */
	__drop(ckptbase, name, strlen(name));

	/* Add a new checkpoint entry at the end of the list. */
	WT_CKPT_FOREACH(ckptbase, ckpt)
		;
	WT_ERR(__wt_strdup(session, name, &ckpt->name));
	F_SET(ckpt, WT_CKPT_ADD);

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
		WT_CKPT_FOREACH(ckptbase, deleted)
			if (F_ISSET(deleted, WT_CKPT_DELETE))
				WT_ERR(__wt_session_lock_checkpoint(session,
				    deleted->name, WT_BTREE_EXCLUSIVE));

	/* Flush the file from the cache, creating the checkpoint. */
	WT_ERR(__wt_bt_cache_flush(
	    session, ckptbase, cfg == NULL ? WT_SYNC_DISCARD : WT_SYNC, force));

	/* If there was a checkpoint, update the metadata and resolve it. */
	if (ckpt->raw.data == NULL) {
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
		if (WT_META_TRACKING(session) && cfg != NULL) {
			WT_ERR(__wt_meta_track_checkpoint(session));
			tracked = 1;
		} else
			WT_ERR(__wt_bm_checkpoint_resolve(session));
	}

err:	__wt_meta_ckptlist_free(session, ckptbase);

	__wt_free(session, name_alloc);

	return (ret);
}
