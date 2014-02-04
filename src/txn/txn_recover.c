/*-
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
	struct {
		const char *uri;	/* File URI. */
		WT_CURSOR *c;		/* Cursor used for recovery. */
		WT_LSN ckpt_lsn;	/* File's checkpoint LSN. */
	} *files;
	size_t file_alloc;		/* Allocated size of files array. */
	u_int max_fileid;		/* Maximum file ID seen. */
	u_int nfiles;			/* Number of files in the metadata. */

	WT_LSN ckpt_lsn;		/* Start LSN for main recovery loop. */

	int modified;			/* Did recovery make any changes? */
	int metadata_only;		/*
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
    WT_LSN *lsnp, u_int id, int duplicate, WT_CURSOR **cp)
{
	WT_CURSOR *c;
	const char *cfg[] = { WT_CONFIG_BASE(session, session_open_cursor),
	    "overwrite", NULL };
	int metadata_op;

	c = NULL;

	/*
	 * Metadata operations have an id of 0.  Match operations based
	 * on the id and the current pass of recovery for metadata.
	 *
	 * Only apply operations in the correct metadata phase, and if the LSN
	 * is more recent than the last checkpoint.  If there is no entry for a
	 * file, assume it was dropped.
	 */
	metadata_op = (id == 0);
	if (r->metadata_only != metadata_op ||
	    LOG_CMP(lsnp, &r->files[id].ckpt_lsn) < 0)
		;
	else if (id > r->max_fileid)
		r->max_fileid = id;
	else if (id >= r->nfiles || r->files[id].uri == NULL)
		WT_VERBOSE_RET(session, recovery,
		    "No file found with ID %u (max %u)", id, r->nfiles);
	else if ((c = r->files[id].c) == NULL) {
		WT_RET(__wt_open_cursor(
		    session, r->files[id].uri, NULL, cfg, &c));
		r->files[id].c = c;
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
	WT_ERR(__recovery_cursor(					\
	    (session), (r), (lsnp), (fileid), 0, (cp)));		\
	WT_VERBOSE_ERR((session), recovery,				\
	    "%s op %d to file %d at LSN %u/%" PRIuMAX,			\
	    (cursor == NULL) ? "Skipping" : "Applying",			\
	    optype, fileid, lsnp->file, (uintmax_t)lsnp->offset);	\
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
		if (start_recno == 0) {
			start = NULL;
			stop = cursor;
		} else if (stop_recno == 0) {
			start = cursor;
			stop = NULL;
		} else {
			start = cursor;
			WT_ERR(__recovery_cursor(
			    session, r, lsnp, fileid, 1, &stop));
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
		case TXN_TRUNC_ALL:
			/* Both cursors stay NULL. */
			break;
		case TXN_TRUNC_BOTH:
			start = cursor;
			WT_ERR(__recovery_cursor(
			    session, r, lsnp, fileid, 1, &stop));
			break;
		case TXN_TRUNC_START:
			start = cursor;
			break;
		case TXN_TRUNC_STOP:
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

	r->modified = 1;

err:	if (ret != 0)
		__wt_err(session, ret,
		    "Operation failed during recovery");
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
__txn_log_recover(
    WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *lsnp, void *cookie)
{
	WT_RECOVERY *r;
	const uint8_t *end, *p;
	uint64_t txnid;
	uint32_t rectype;

	r = cookie;
	p = (const uint8_t *)logrec->data + offsetof(WT_LOG_RECORD, record);
	end = (const uint8_t *)logrec->data + logrec->size;

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
	uint32_t fileid;

	WT_RET(__wt_config_getones(r->session, config, "id", &cval));
	fileid = (uint32_t)cval.val;

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
		INIT_LSN(&lsn);
	else if (sscanf(cval.str, "(%" PRIu32 ",%" PRIdMAX ")",
	    &lsn.file, (intmax_t*)&lsn.offset) != 2)
		WT_RET_MSG(r->session, EINVAL,
		    "Failed to parse checkpoint LSN '%.*s'",
		    (int)cval.len, cval.str);
	r->files[fileid].ckpt_lsn = lsn;

	WT_VERBOSE_RET(r->session, recovery,
	    "Recovering %s with id %u @ (%" PRIu32 ", %" PRIu64 ")",
	    uri, fileid, lsn.file, lsn.offset);

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
	WT_DECL_RET;
	WT_CURSOR *c;
	const char *uri, *config;
	int cmp;

	/* Scan through all files in the metadata. */
	c = r->files[0].c;
	c->set_key(c, "file:");
	if ((ret = c->search_near(c, &cmp)) != 0) {
		/* Is the metadata empty? */
		if (ret == WT_NOTFOUND)
			ret = 0;
		goto err;
	}
	if (cmp < 0)
		WT_ERR_NOTFOUND_OK(c->next(c));
	for (; ret == 0; ret = c->next(c)) {
		WT_ERR(c->get_key(c, &uri));
		if (!WT_PREFIX_MATCH(uri, "file:"))
			break;
		WT_ERR(c->get_value(c, &config));
		WT_ERR(__recovery_setup_file(r, uri, config));
	}
	WT_ERR_NOTFOUND_OK(ret);

err:	r->max_fileid = r->nfiles;
	return (ret);
}

/*
 * __wt_txn_recover --
 *	Run recovery.
 */
int
__wt_txn_recover(WT_SESSION_IMPL *default_session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_RECOVERY r;
	WT_SESSION_IMPL *session;
	const char *config;
	int modified;

	conn = S2C(default_session);
	WT_CLEAR(r);
	INIT_LSN(&r.ckpt_lsn);

	/* We need a real session for recovery. */
	WT_RET(__wt_open_session(conn, 0, NULL, NULL, &session));
	F_SET(session, WT_SESSION_LOGGING_DISABLED);
	r.session = session;

	WT_ERR(__wt_metadata_search(session, WT_METADATA_URI, &config));
	WT_ERR(__recovery_setup_file(&r, WT_METADATA_URI, config));
	WT_ERR(__wt_metadata_cursor(session, NULL, &r.files[0].c));

	/*
	 * First, do a full pass through the log to recover the metadata,
	 * and establish the last checkpoint LSN.
	 */
	r.metadata_only = 1;
	WT_ERR(__wt_log_scan(
	    session, NULL, WT_LOGSCAN_FIRST, __txn_log_recover, &r));

	WT_ASSERT(session, LOG_CMP(&r.ckpt_lsn, &conn->log->first_lsn) >= 0);

	WT_ERR(__recovery_file_scan(&r));

	/*
	 * Now, recover all the files apart from the metadata.
	 * Pass WT_LOGSCAN_RECOVER so that old logs get truncated.
	 */
	r.metadata_only = 0;
	WT_VERBOSE_ERR(session, recovery,
	    "Main recovery loop: starting at %u/%" PRIuMAX,
	    r.ckpt_lsn.file, (uintmax_t)r.ckpt_lsn.offset);
	if (IS_INIT_LSN(&r.ckpt_lsn))
		WT_ERR(__wt_log_scan(session, NULL,
		    WT_LOGSCAN_FIRST | WT_LOGSCAN_RECOVER,
		    __txn_log_recover, &r));
	else
		WT_ERR(__wt_log_scan(session, &r.ckpt_lsn,
		    WT_LOGSCAN_RECOVER,
		    __txn_log_recover, &r));

	conn->next_file_id = r.max_fileid;

err:	modified = r.modified;
	WT_TRET(__recovery_free(&r));
	__wt_free(session, config);
	WT_TRET(session->iface.close(&session->iface, NULL));

	/*
	 * If recovery ran successfully and modified something, log a
	 * checkpoint.
	 */
	if (ret == 0 && modified)
		ret = __wt_txn_checkpoint_log(
		    default_session, 1, WT_TXN_LOG_CKPT_STOP, NULL);

	return (ret);
}
