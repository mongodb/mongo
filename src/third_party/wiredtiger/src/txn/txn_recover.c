/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/* State maintained during recovery. */
typedef struct {
	WT_SESSION_IMPL *session;

	/* Files from the metadata, indexed by file ID. */
	struct WT_RECOVERY_FILE {
		const char *uri;	/* File URI. */
		WT_CURSOR *c;		/* Cursor used for recovery. */
		WT_LSN ckpt_lsn;	/* File's checkpoint LSN. */
	} *files;
	size_t file_alloc;		/* Allocated size of files array. */
	u_int max_fileid;		/* Maximum file ID seen. */
	u_int nfiles;			/* Number of files in the metadata. */

	WT_LSN ckpt_lsn;		/* Start LSN for main recovery loop. */

	bool missing;			/* Were there missing files? */
	bool metadata_only;		/*
					 * Set during the first recovery pass,
					 * when only the metadata is recovered.
					 */
} WT_RECOVERY;

/*
 * __recovery_cursor --
 *	Get a cursor for a recovery operation.
 */
static int
__recovery_cursor(WT_SESSION_IMPL *session, WT_RECOVERY *r,
    WT_LSN *lsnp, u_int id, bool duplicate, WT_CURSOR **cp)
{
	WT_CURSOR *c;
	bool metadata_op;
	const char *cfg[] = { WT_CONFIG_BASE(
	    session, WT_SESSION_open_cursor), "overwrite", NULL };

	c = NULL;

	/*
	 * Metadata operations have an id of 0.  Match operations based
	 * on the id and the current pass of recovery for metadata.
	 *
	 * Only apply operations in the correct metadata phase, and if the LSN
	 * is more recent than the last checkpoint.  If there is no entry for a
	 * file, assume it was dropped or missing after a hot backup.
	 */
	metadata_op = id == WT_METAFILE_ID;
	if (r->metadata_only != metadata_op)
		;
	else if (id >= r->nfiles || r->files[id].uri == NULL) {
		/* If a file is missing, output a verbose message once. */
		if (!r->missing)
			WT_RET(__wt_verbose(session, WT_VERB_RECOVERY,
			    "No file found with ID %u (max %u)",
			    id, r->nfiles));
		r->missing = true;
	} else if (__wt_log_cmp(lsnp, &r->files[id].ckpt_lsn) >= 0) {
		/*
		 * We're going to apply the operation.  Get the cursor, opening
		 * one if none is cached.
		 */
		if ((c = r->files[id].c) == NULL) {
			WT_RET(__wt_open_cursor(
			    session, r->files[id].uri, NULL, cfg, &c));
			r->files[id].c = c;
		}
	}

	if (duplicate && c != NULL)
		WT_RET(__wt_open_cursor(
		    session, r->files[id].uri, NULL, cfg, &c));

	*cp = c;
	return (0);
}

/*
 * Helper to a cursor if this operation is to be applied during recovery.
 */
#define	GET_RECOVERY_CURSOR(session, r, lsnp, fileid, cp)		\
	WT_ERR(__recovery_cursor(session, r, lsnp, fileid, false, cp));	\
	WT_ERR(__wt_verbose(session, WT_VERB_RECOVERY,			\
	    "%s op %" PRIu32 " to file %" PRIu32 " at LSN %" PRIu32	\
	    "/%" PRIu32,						\
	    cursor == NULL ? "Skipping" : "Applying",			\
	    optype, fileid, lsnp->l.file, lsnp->l.offset));		\
	if (cursor == NULL)						\
		break

/*
 * __txn_op_apply --
 *	Apply a transactional operation during recovery.
 */
static int
__txn_op_apply(
    WT_RECOVERY *r, WT_LSN *lsnp, const uint8_t **pp, const uint8_t *end)
{
	WT_CURSOR *cursor, *start, *stop;
	WT_DECL_RET;
	WT_ITEM key, start_key, stop_key, value;
	WT_SESSION_IMPL *session;
	uint64_t recno, start_recno, stop_recno;
	uint32_t fileid, mode, optype, opsize;

	session = r->session;
	cursor = NULL;

	/* Peek at the size and the type. */
	WT_ERR(__wt_logop_read(session, pp, end, &optype, &opsize));
	end = *pp + opsize;

	switch (optype) {
	case WT_LOGOP_COL_PUT:
		WT_ERR(__wt_logop_col_put_unpack(session, pp, end,
		    &fileid, &recno, &value));
		GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);
		cursor->set_key(cursor, recno);
		__wt_cursor_set_raw_value(cursor, &value);
		WT_ERR(cursor->insert(cursor));
		break;

	case WT_LOGOP_COL_REMOVE:
		WT_ERR(__wt_logop_col_remove_unpack(session, pp, end,
		    &fileid, &recno));
		GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);
		cursor->set_key(cursor, recno);
		WT_ERR(cursor->remove(cursor));
		break;

	case WT_LOGOP_COL_TRUNCATE:
		WT_ERR(__wt_logop_col_truncate_unpack(session, pp, end,
		    &fileid, &start_recno, &stop_recno));
		GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);

		/* Set up the cursors. */
		if (start_recno == WT_RECNO_OOB) {
			start = NULL;
			stop = cursor;
		} else if (stop_recno == WT_RECNO_OOB) {
			start = cursor;
			stop = NULL;
		} else {
			start = cursor;
			WT_ERR(__recovery_cursor(
			    session, r, lsnp, fileid, true, &stop));
		}

		/* Set the keys. */
		if (start != NULL)
			start->set_key(start, start_recno);
		if (stop != NULL)
			stop->set_key(stop, stop_recno);

		WT_TRET(session->iface.truncate(&session->iface, NULL,
		    start, stop, NULL));
		/* If we opened a duplicate cursor, close it now. */
		if (stop != NULL && stop != cursor)
			WT_TRET(stop->close(stop));
		WT_ERR(ret);
		break;

	case WT_LOGOP_ROW_PUT:
		WT_ERR(__wt_logop_row_put_unpack(session, pp, end,
		    &fileid, &key, &value));
		GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);
		__wt_cursor_set_raw_key(cursor, &key);
		__wt_cursor_set_raw_value(cursor, &value);
		WT_ERR(cursor->insert(cursor));
		break;

	case WT_LOGOP_ROW_REMOVE:
		WT_ERR(__wt_logop_row_remove_unpack(session, pp, end,
		    &fileid, &key));
		GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);
		__wt_cursor_set_raw_key(cursor, &key);
		WT_ERR(cursor->remove(cursor));
		break;

	case WT_LOGOP_ROW_TRUNCATE:
		WT_ERR(__wt_logop_row_truncate_unpack(session, pp, end,
		    &fileid, &start_key, &stop_key, &mode));
		GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);
		/* Set up the cursors. */
		start = stop = NULL;
		switch (mode) {
		case WT_TXN_TRUNC_ALL:
			/* Both cursors stay NULL. */
			break;
		case WT_TXN_TRUNC_BOTH:
			start = cursor;
			WT_ERR(__recovery_cursor(
			    session, r, lsnp, fileid, true, &stop));
			break;
		case WT_TXN_TRUNC_START:
			start = cursor;
			break;
		case WT_TXN_TRUNC_STOP:
			stop = cursor;
			break;

		WT_ILLEGAL_VALUE_ERR(session);
		}

		/* Set the keys. */
		if (start != NULL)
			__wt_cursor_set_raw_key(start, &start_key);
		if (stop != NULL)
			__wt_cursor_set_raw_key(stop, &stop_key);

		WT_TRET(session->iface.truncate(&session->iface, NULL,
		    start, stop, NULL));
		/* If we opened a duplicate cursor, close it now. */
		if (stop != NULL && stop != cursor)
			WT_TRET(stop->close(stop));
		WT_ERR(ret);
		break;

	WT_ILLEGAL_VALUE_ERR(session);
	}

	/* Reset the cursor so it doesn't block eviction. */
	if (cursor != NULL)
		WT_ERR(cursor->reset(cursor));

err:	if (ret != 0)
		__wt_err(session, ret, "Operation failed during recovery");
	return (ret);
}

/*
 * __txn_commit_apply --
 *	Apply a commit record during recovery.
 */
static int
__txn_commit_apply(
    WT_RECOVERY *r, WT_LSN *lsnp, const uint8_t **pp, const uint8_t *end)
{
	WT_UNUSED(lsnp);

	/* The logging subsystem zero-pads records. */
	while (*pp < end && **pp)
		WT_RET(__txn_op_apply(r, lsnp, pp, end));

	return (0);
}

/*
 * __txn_log_recover --
 *	Roll the log forward to recover committed changes.
 */
static int
__txn_log_recover(WT_SESSION_IMPL *session,
    WT_ITEM *logrec, WT_LSN *lsnp, WT_LSN *next_lsnp,
    void *cookie, int firstrecord)
{
	WT_RECOVERY *r;
	const uint8_t *end, *p;
	uint64_t txnid;
	uint32_t rectype;

	WT_UNUSED(next_lsnp);
	r = cookie;
	p = WT_LOG_SKIP_HEADER(logrec->data);
	end = (const uint8_t *)logrec->data + logrec->size;
	WT_UNUSED(firstrecord);

	/* First, peek at the log record type. */
	WT_RET(__wt_logrec_read(session, &p, end, &rectype));

	switch (rectype) {
	case WT_LOGREC_CHECKPOINT:
		if (r->metadata_only)
			WT_RET(__wt_txn_checkpoint_logread(
			    session, &p, end, &r->ckpt_lsn));
		break;

	case WT_LOGREC_COMMIT:
		WT_RET(__wt_vunpack_uint(&p, WT_PTRDIFF(end, p), &txnid));
		WT_UNUSED(txnid);
		WT_RET(__txn_commit_apply(r, lsnp, &p, end));
		break;
	}

	return (0);
}

/*
 * __recovery_setup_file --
 *	Set up the recovery slot for a file.
 */
static int
__recovery_setup_file(WT_RECOVERY *r, const char *uri, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_LSN lsn;
	uint32_t fileid, lsnfile, lsnoffset;

	WT_RET(__wt_config_getones(r->session, config, "id", &cval));
	fileid = (uint32_t)cval.val;

	/* Track the largest file ID we have seen. */
	if (fileid > r->max_fileid)
		r->max_fileid = fileid;

	if (r->nfiles <= fileid) {
		WT_RET(__wt_realloc_def(
		    r->session, &r->file_alloc, fileid + 1, &r->files));
		r->nfiles = fileid + 1;
	}

	WT_RET(__wt_strdup(r->session, uri, &r->files[fileid].uri));
	WT_RET(
	    __wt_config_getones(r->session, config, "checkpoint_lsn", &cval));
	/* If there is checkpoint logged for the file, apply everything. */
	if (cval.type != WT_CONFIG_ITEM_STRUCT)
		WT_INIT_LSN(&lsn);
	else if (sscanf(cval.str,
	    "(%" SCNu32 ",%" SCNu32 ")", &lsnfile, &lsnoffset) == 2)
		WT_SET_LSN(&lsn, lsnfile, lsnoffset);
	else
		WT_RET_MSG(r->session, EINVAL,
		    "Failed to parse checkpoint LSN '%.*s'",
		    (int)cval.len, cval.str);
	r->files[fileid].ckpt_lsn = lsn;

	WT_RET(__wt_verbose(r->session, WT_VERB_RECOVERY,
	    "Recovering %s with id %" PRIu32 " @ (%" PRIu32 ", %" PRIu32 ")",
	    uri, fileid, lsn.l.file, lsn.l.offset));

	return (0);

}

/*
 * __recovery_free --
 *	Free the recovery state.
 */
static int
__recovery_free(WT_RECOVERY *r)
{
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int i;

	session = r->session;
	for (i = 0; i < r->nfiles; i++) {
		__wt_free(session, r->files[i].uri);
		if ((c = r->files[i].c) != NULL)
			WT_TRET(c->close(c));
	}

	__wt_free(session, r->files);
	return (ret);
}

/*
 * __recovery_file_scan --
 *	Scan the files referenced from the metadata and gather information
 *	about them for recovery.
 */
static int
__recovery_file_scan(WT_RECOVERY *r)
{
	WT_CURSOR *c;
	WT_DECL_RET;
	int cmp;
	const char *uri, *config;

	/* Scan through all files in the metadata. */
	c = r->files[0].c;
	c->set_key(c, "file:");
	if ((ret = c->search_near(c, &cmp)) != 0) {
		/* Is the metadata empty? */
		WT_RET_NOTFOUND_OK(ret);
		return (0);
	}
	if (cmp < 0)
		WT_RET_NOTFOUND_OK(c->next(c));
	for (; ret == 0; ret = c->next(c)) {
		WT_RET(c->get_key(c, &uri));
		if (!WT_PREFIX_MATCH(uri, "file:"))
			break;
		WT_RET(c->get_value(c, &config));
		WT_RET(__recovery_setup_file(r, uri, config));
	}
	WT_RET_NOTFOUND_OK(ret);
	return (0);
}

/*
 * __wt_txn_recover --
 *	Run recovery.
 */
int
__wt_txn_recover(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *metac;
	WT_DECL_RET;
	WT_RECOVERY r;
	struct WT_RECOVERY_FILE *metafile;
	char *config;
	bool eviction_started, needs_rec, was_backup;

	conn = S2C(session);
	WT_CLEAR(r);
	WT_INIT_LSN(&r.ckpt_lsn);
	eviction_started = false;
	was_backup = F_ISSET(conn, WT_CONN_WAS_BACKUP);

	/* We need a real session for recovery. */
	WT_RET(__wt_open_internal_session(conn, "txn-recover",
	    false, WT_SESSION_NO_LOGGING, &session));
	r.session = session;

	F_SET(conn, WT_CONN_RECOVERING);
	WT_ERR(__wt_metadata_search(session, WT_METAFILE_URI, &config));
	WT_ERR(__recovery_setup_file(&r, WT_METAFILE_URI, config));
	WT_ERR(__wt_metadata_cursor_open(session, NULL, &metac));
	metafile = &r.files[WT_METAFILE_ID];
	metafile->c = metac;

	/*
	 * If no log was found (including if logging is disabled), or if the
	 * last checkpoint was done with logging disabled, recovery should not
	 * run.  Scan the metadata to figure out the largest file ID.
	 */
	if (!FLD_ISSET(S2C(session)->log_flags, WT_CONN_LOG_EXISTED) ||
	    WT_IS_MAX_LSN(&metafile->ckpt_lsn)) {
		WT_ERR(__recovery_file_scan(&r));
		conn->next_file_id = r.max_fileid;
		goto done;
	}

	/*
	 * First, do a pass through the log to recover the metadata, and
	 * establish the last checkpoint LSN.  Skip this when opening a hot
	 * backup: we already have the correct metadata in that case.
	 */
	if (!was_backup) {
		r.metadata_only = true;
		/*
		 * If this is a read-only connection, check if the checkpoint
		 * LSN in the metadata file is up to date, indicating a clean
		 * shutdown.
		 */
		if (F_ISSET(conn, WT_CONN_READONLY)) {
			WT_ERR(__wt_log_needs_recovery(
			    session, &metafile->ckpt_lsn, &needs_rec));
			if (needs_rec)
				WT_ERR_MSG(session, WT_RUN_RECOVERY,
				    "Read-only database needs recovery");
		}
		if (WT_IS_INIT_LSN(&metafile->ckpt_lsn))
			WT_ERR(__wt_log_scan(session,
			    NULL, WT_LOGSCAN_FIRST, __txn_log_recover, &r));
		else {
			/*
			 * Start at the last checkpoint LSN referenced in the
			 * metadata.  If we see the end of a checkpoint while
			 * scanning, we will change the full scan to start from
			 * there.
			 */
			r.ckpt_lsn = metafile->ckpt_lsn;
			ret = __wt_log_scan(session,
			    &metafile->ckpt_lsn, 0, __txn_log_recover, &r);
			if (ret == ENOENT)
				ret = 0;
			WT_ERR(ret);
		}
	}

	/* Scan the metadata to find the live files and their IDs. */
	WT_ERR(__recovery_file_scan(&r));

	/*
	 * We no longer need the metadata cursor: close it to avoid pinning any
	 * resources that could block eviction during recovery.
	 */
	r.files[0].c = NULL;
	WT_ERR(metac->close(metac));

	/*
	 * Now, recover all the files apart from the metadata.
	 * Pass WT_LOGSCAN_RECOVER so that old logs get truncated.
	 */
	r.metadata_only = false;
	WT_ERR(__wt_verbose(session, WT_VERB_RECOVERY,
	    "Main recovery loop: starting at %" PRIu32 "/%" PRIu32,
	    r.ckpt_lsn.l.file, r.ckpt_lsn.l.offset));
	WT_ERR(__wt_log_needs_recovery(session, &r.ckpt_lsn, &needs_rec));
	/*
	 * Check if the database was shut down cleanly.  If not
	 * return an error if the user does not want automatic
	 * recovery.
	 */
	if (needs_rec &&
	    (FLD_ISSET(conn->log_flags, WT_CONN_LOG_RECOVER_ERR) ||
	     F_ISSET(conn, WT_CONN_READONLY))) {
		if (F_ISSET(conn, WT_CONN_READONLY))
			WT_ERR_MSG(session, WT_RUN_RECOVERY,
			    "Read-only database needs recovery");
		WT_ERR(WT_RUN_RECOVERY);
	}

	if (F_ISSET(conn, WT_CONN_READONLY))
		goto done;

	/*
	 * Recovery can touch more data than fits in cache, so it relies on
	 * regular eviction to manage paging.  Start eviction threads for
	 * recovery without LAS cursors.
	 */
	WT_ERR(__wt_evict_create(session));
	eviction_started = true;

	/*
	 * Always run recovery even if it was a clean shutdown only if
	 * this is not a read-only connection.
	 * We can consider skipping it in the future.
	 */
	if (WT_IS_INIT_LSN(&r.ckpt_lsn))
		WT_ERR(__wt_log_scan(session, NULL,
		    WT_LOGSCAN_FIRST | WT_LOGSCAN_RECOVER,
		    __txn_log_recover, &r));
	else {
		ret = __wt_log_scan(session, &r.ckpt_lsn,
		    WT_LOGSCAN_RECOVER, __txn_log_recover, &r);
		if (ret == ENOENT)
			ret = 0;
		WT_ERR(ret);
	}

	conn->next_file_id = r.max_fileid;

	/*
	 * If recovery ran successfully forcibly log a checkpoint so the next
	 * open is fast and keep the metadata up to date with the checkpoint
	 * LSN and archiving.
	 */
	WT_ERR(session->iface.checkpoint(&session->iface, "force=1"));

done:	FLD_SET(conn->log_flags, WT_CONN_LOG_RECOVER_DONE);
err:	WT_TRET(__recovery_free(&r));
	__wt_free(session, config);

	if (ret != 0)
		__wt_err(session, ret, "Recovery failed");

	/*
	 * Destroy the eviction threads that were started in support of
	 * recovery.  They will be restarted once the lookaside table is
	 * created.
	 */
	if (eviction_started)
		WT_TRET(__wt_evict_destroy(session));

	WT_TRET(session->iface.close(&session->iface, NULL));
	F_CLR(conn, WT_CONN_RECOVERING);

	return (ret);
}
