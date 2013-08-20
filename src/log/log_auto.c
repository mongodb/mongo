/* DO NOT EDIT: automatically built by dist/log.py. */

#include "wt_internal.h"

int
__wt_logrec_alloc(WT_SESSION_IMPL *session, WT_ITEM **logrecp)
{
	WT_ITEM *logrec;

	WT_RET(__wt_scr_alloc(session, LOG_ALIGN, &logrec));
	WT_CLEAR(*(WT_LOG_RECORD *)logrec->data);
	logrec->size = offsetof(WT_LOG_RECORD, record);

	*logrecp = logrec;
	return (0);
}

void
__wt_logrec_free(WT_SESSION_IMPL *session, WT_ITEM **logrecp)
{
	WT_UNUSED(session);
	__wt_scr_free(logrecp);
}

int
__wt_logrec_read(WT_SESSION_IMPL *session,
    const uint8_t **pp, const uint8_t *end, uint32_t *rectypep)
{
	uint64_t rectype;

	WT_UNUSED(session);
	WT_RET(__wt_vunpack_uint(pp, WT_PTRDIFF(end, *pp), &rectype));
	*rectypep = (uint32_t)rectype;
	return (0);
}

int
__wt_logop_read(WT_SESSION_IMPL *session,
    const uint8_t **pp, const uint8_t *end,
    uint32_t *optypep, uint32_t *opsizep)
{
	return (__wt_struct_unpack(
	    session, *pp, WT_PTRDIFF(end, *pp), "II", optypep, opsizep));
}

int
__wt_logop_col_put_pack(
    WT_SESSION_IMPL *session, WT_ITEM *logrec,
    const char *uri, uint64_t recno, WT_ITEM *value)
{
	const char *fmt = WT_UNCHECKED_STRING(IISru);
	size_t size;
	uint32_t optype, recsize;

	optype = WT_LOGOP_COL_PUT;
	WT_RET(__wt_struct_size(session, &size, fmt,
	    optype, 0, uri, recno, value));

	size += __wt_vsize_uint(size) - 1;
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, uri, recno, value));

	logrec->size += size;
	return (0);
}

int
__wt_logop_col_put_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
    const char **urip, uint64_t *recnop, WT_ITEM *valuep)
{
	const char *fmt = WT_UNCHECKED_STRING(IISru);
	uint32_t optype, size;

	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &optype, &size, urip, recnop, valuep));
	WT_ASSERT(session, optype == WT_LOGOP_COL_PUT);

	*pp += size;
	return (0);
}

int
__wt_logop_col_put_print(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	const char *uri;
	uint64_t recno;
	WT_ITEM value;

	WT_RET(__wt_logop_col_put_unpack(
	    session, pp, end, &uri, &recno, &value));

	fprintf(out, "\t" "uri: %s\n", uri);
	fprintf(out, "\t" "recno: %" PRIu64 "\n", recno);
	fprintf(out, "\t" "value: %.*s\n",
	    (int)value.size, (const char *)value.data);
	return (0);
}

int
__wt_logop_col_remove_pack(
    WT_SESSION_IMPL *session, WT_ITEM *logrec,
    const char *uri, uint64_t recno)
{
	const char *fmt = WT_UNCHECKED_STRING(IISr);
	size_t size;
	uint32_t optype, recsize;

	optype = WT_LOGOP_COL_REMOVE;
	WT_RET(__wt_struct_size(session, &size, fmt,
	    optype, 0, uri, recno));

	size += __wt_vsize_uint(size) - 1;
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, uri, recno));

	logrec->size += size;
	return (0);
}

int
__wt_logop_col_remove_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
    const char **urip, uint64_t *recnop)
{
	const char *fmt = WT_UNCHECKED_STRING(IISr);
	uint32_t optype, size;

	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &optype, &size, urip, recnop));
	WT_ASSERT(session, optype == WT_LOGOP_COL_REMOVE);

	*pp += size;
	return (0);
}

int
__wt_logop_col_remove_print(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	const char *uri;
	uint64_t recno;

	WT_RET(__wt_logop_col_remove_unpack(
	    session, pp, end, &uri, &recno));

	fprintf(out, "\t" "uri: %s\n", uri);
	fprintf(out, "\t" "recno: %" PRIu64 "\n", recno);
	return (0);
}

int
__wt_logop_row_put_pack(
    WT_SESSION_IMPL *session, WT_ITEM *logrec,
    const char *uri, WT_ITEM *key, WT_ITEM *value)
{
	const char *fmt = WT_UNCHECKED_STRING(IISuu);
	size_t size;
	uint32_t optype, recsize;

	optype = WT_LOGOP_ROW_PUT;
	WT_RET(__wt_struct_size(session, &size, fmt,
	    optype, 0, uri, key, value));

	size += __wt_vsize_uint(size) - 1;
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, uri, key, value));

	logrec->size += size;
	return (0);
}

int
__wt_logop_row_put_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
    const char **urip, WT_ITEM *keyp, WT_ITEM *valuep)
{
	const char *fmt = WT_UNCHECKED_STRING(IISuu);
	uint32_t optype, size;

	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &optype, &size, urip, keyp, valuep));
	WT_ASSERT(session, optype == WT_LOGOP_ROW_PUT);

	*pp += size;
	return (0);
}

int
__wt_logop_row_put_print(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	const char *uri;
	WT_ITEM key;
	WT_ITEM value;

	WT_RET(__wt_logop_row_put_unpack(
	    session, pp, end, &uri, &key, &value));

	fprintf(out, "\t" "uri: %s\n", uri);
	fprintf(out, "\t" "key: %.*s\n",
	    (int)key.size, (const char *)key.data);
	fprintf(out, "\t" "value: %.*s\n",
	    (int)value.size, (const char *)value.data);
	return (0);
}

int
__wt_logop_row_remove_pack(
    WT_SESSION_IMPL *session, WT_ITEM *logrec,
    const char *uri, WT_ITEM *key)
{
	const char *fmt = WT_UNCHECKED_STRING(IISu);
	size_t size;
	uint32_t optype, recsize;

	optype = WT_LOGOP_ROW_REMOVE;
	WT_RET(__wt_struct_size(session, &size, fmt,
	    optype, 0, uri, key));

	size += __wt_vsize_uint(size) - 1;
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, uri, key));

	logrec->size += size;
	return (0);
}

int
__wt_logop_row_remove_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
    const char **urip, WT_ITEM *keyp)
{
	const char *fmt = WT_UNCHECKED_STRING(IISu);
	uint32_t optype, size;

	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &optype, &size, urip, keyp));
	WT_ASSERT(session, optype == WT_LOGOP_ROW_REMOVE);

	*pp += size;
	return (0);
}

int
__wt_logop_row_remove_print(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	const char *uri;
	WT_ITEM key;

	WT_RET(__wt_logop_row_remove_unpack(
	    session, pp, end, &uri, &key));

	fprintf(out, "\t" "uri: %s\n", uri);
	fprintf(out, "\t" "key: %.*s\n",
	    (int)key.size, (const char *)key.data);
	return (0);
}
