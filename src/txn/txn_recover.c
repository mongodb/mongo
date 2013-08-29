/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/* Recovery state. */
typedef struct {
	WT_SESSION_IMPL *session;

	struct {
		const char *uri;
		WT_CURSOR *c;
		WT_LSN ckpt_lsn;
	} *files;

	size_t file_alloc;
	u_int max_fileid, nfiles;

	int metadata_only;
} WT_RECOVERY;

static int
__recovery_cursor(WT_SESSION_IMPL *session, WT_RECOVERY *r,
    WT_LSN *lsnp, u_int id, WT_CURSOR **cp)
{
	WT_CURSOR *c;
	const char *cfg[] = { WT_CONFIG_BASE(session, session_open_cursor),
	    "overwrite", NULL };

	c = NULL;

	/*
	 * Only apply operations in the correct metadata phase, and if the LSN
	 * is more recent than th last checkpoint.  If there is no entry for a
	 * file, assume it was dropped.
	 */
	if (r->metadata_only != (id == 0) ||
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

	*cp = c;
	return (0);
}

static int
__txn_op_apply(
    WT_RECOVERY *r, WT_LSN *lsnp, const uint8_t **pp, const uint8_t *end)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_ITEM key, value;
	WT_SESSION_IMPL *session;
	uint64_t recno;
	uint32_t fileid, optype, opsize;

	session = r->session;

	/* Peek at the size and the type. */
	WT_RET(__wt_logop_read(session, pp, end, &optype, &opsize));
	end = *pp + opsize;

	switch (optype) {
	case WT_LOGOP_COL_PUT:
		WT_RET(__wt_logop_col_put_unpack(session, pp, end,
		    &fileid, &recno, &value));
		WT_RET(__recovery_cursor(session, r, lsnp, fileid, &cursor));
		if (cursor == NULL)
			break;
		cursor->set_key(cursor, recno);
		__wt_cursor_set_raw_value(cursor, &value);
		WT_TRET(cursor->insert(cursor));
		break;

	case WT_LOGOP_COL_REMOVE:
		WT_RET(__wt_logop_col_remove_unpack(session, pp, end,
		    &fileid, &recno));
		WT_RET(__recovery_cursor(session, r, lsnp, fileid, &cursor));
		if (cursor == NULL)
			break;
		cursor->set_key(cursor, recno);
		WT_TRET(cursor->remove(cursor));
		break;

	case WT_LOGOP_ROW_PUT:
		WT_RET(__wt_logop_row_put_unpack(session, pp, end,
		    &fileid, &key, &value));
		WT_RET(__recovery_cursor(session, r, lsnp, fileid, &cursor));
		if (cursor == NULL)
			break;
		__wt_cursor_set_raw_key(cursor, &key);
		__wt_cursor_set_raw_value(cursor, &value);
		WT_TRET(cursor->insert(cursor));
		break;

	case WT_LOGOP_ROW_REMOVE:
		WT_RET(__wt_logop_row_remove_unpack(session, pp, end,
		    &fileid, &key));
		WT_RET(__recovery_cursor(session, r, lsnp, fileid, &cursor));
		if (cursor == NULL)
			break;
		__wt_cursor_set_raw_key(cursor, &key);
		WT_TRET(cursor->remove(cursor));
		break;

	WT_ILLEGAL_VALUE(session);
	}

	if (ret != 0)
		__wt_err(session, ret,
		    "Operation failed during recovery");
	return (ret);
}

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
	const uint8_t *end, *p;
	uint32_t rectype;

	p = (const uint8_t *)logrec->data + offsetof(WT_LOG_RECORD, record);
	end = (const uint8_t *)logrec->data + logrec->size;

	/* First, peek at the log record type. */
	WT_RET(__wt_logrec_read(session, &p, end, &rectype));

	switch (rectype) {
	case WT_LOGREC_COMMIT:
		WT_RET(__txn_commit_apply(cookie, lsnp, &p, end));
		break;
	}

	return (0);
}

/*
 * __recovery_setup_slot --
 *	Set up the recovery slot for a file.
 */
static int
__recovery_setup_file(WT_RECOVERY *r,
    const char *uri, const char *config, WT_LSN *start_lsnp)
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
	else if (sscanf(cval.str, "(%" PRIu32 ",%" PRIuMAX ")",
	    &lsn.file, &lsn.offset) != 2)
		WT_RET_MSG(r->session, EINVAL,
		    "Failed to parse checkpoint LSN '%.*s'",
		    (int)cval.len, cval.str);

	r->files[fileid].ckpt_lsn = lsn;
	if (LOG_CMP(&lsn, start_lsnp) < 0)
		*start_lsnp = lsn;

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
__recovery_file_scan(WT_RECOVERY *r, WT_LSN *start_lsnp)
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
		WT_ERR(__recovery_setup_file(r, uri, config, start_lsnp));
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
	WT_LSN start_lsn;
	WT_RECOVERY r;
	WT_SESSION_IMPL *session;
	const char *config;

	conn = S2C(default_session);
	WT_CLEAR(r);

	/* We need a real session for recovery. */
	WT_RET(__wt_open_session(conn, 0, NULL, NULL, &session));
	F_SET(session, WT_SESSION_NO_LOGGING);
	r.session = session;

	WT_ERR(__wt_metadata_search(session, WT_METADATA_URI, &config));
	MAX_LSN(&start_lsn);
	WT_ERR(__recovery_setup_file(&r, WT_METADATA_URI, config, &start_lsn));
	WT_ERR(__wt_metadata_cursor(session, NULL, &r.files[0].c));

	/* First, recover the metadata, starting from the checkpoint's LSN. */
	r.metadata_only = 1;
	WT_ERR(__wt_log_scan(session, &start_lsn, WT_LOGSCAN_RECOVER,
	    __txn_log_recover, &r));

	WT_ERR(__recovery_file_scan(&r, &start_lsn));

	/* Now, recover all the files apart from the metadata. */
	r.metadata_only = 0;
	if (IS_INIT_LSN(&start_lsn))
		WT_ERR(__wt_log_scan(session, NULL,
		    WT_LOGSCAN_FIRST | WT_LOGSCAN_RECOVER,
		    __txn_log_recover, &r));
	else
		WT_ERR(__wt_log_scan(session, &start_lsn,
		    WT_LOGSCAN_RECOVER, __txn_log_recover, &r));

	conn->next_file_id = r.max_fileid;

err:	__recovery_free(&r);
	__wt_free(session, config);
	WT_TRET(session->iface.close(&session->iface, NULL));

	return (ret);
}
