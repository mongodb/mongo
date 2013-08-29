/* DO NOT EDIT: automatically built by dist/log.py. */

#include "wt_internal.h"

int
__wt_logrec_alloc(WT_SESSION_IMPL *session, size_t size, WT_ITEM **logrecp)
{
	WT_ITEM *logrec;

	WT_RET(__wt_scr_alloc(session, WT_ALIGN(size + 1, LOG_ALIGN), &logrec));
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
    uint32_t fileid, uint64_t recno, WT_ITEM *value)
{
	const char *fmt = WT_UNCHECKED_STRING(IIIru);
	size_t size;
	uint32_t optype, recsize;

	optype = WT_LOGOP_COL_PUT;
	WT_RET(__wt_struct_size(session, &size, fmt,
	    optype, 0, fileid, recno, value));

	size += __wt_vsize_uint(size) - 1;
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, fileid, recno, value));

	logrec->size += size;
	return (0);
}

int
__wt_logop_col_put_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
    uint32_t *fileidp, uint64_t *recnop, WT_ITEM *valuep)
{
	const char *fmt = WT_UNCHECKED_STRING(IIIru);
	uint32_t optype, size;

	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &optype, &size, fileidp, recnop, valuep));
	WT_ASSERT(session, optype == WT_LOGOP_COL_PUT);

	*pp += size;
	return (0);
}

int
__wt_logop_col_put_print(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	uint32_t fileid;
	uint64_t recno;
	WT_ITEM value;

	WT_RET(__wt_logop_col_put_unpack(
	    session, pp, end, &fileid, &recno, &value));

	fprintf(out, "\t" "fileid: %" PRIu32 "\n", fileid);
	fprintf(out, "\t" "recno: %" PRIu64 "\n", recno);
	fprintf(out, "\t" "value: %.*s\n",
	    (int)value.size, (const char *)value.data);
	return (0);
}

int
__wt_logop_col_remove_pack(
    WT_SESSION_IMPL *session, WT_ITEM *logrec,
    uint32_t fileid, uint64_t recno)
{
	const char *fmt = WT_UNCHECKED_STRING(IIIr);
	size_t size;
	uint32_t optype, recsize;

	optype = WT_LOGOP_COL_REMOVE;
	WT_RET(__wt_struct_size(session, &size, fmt,
	    optype, 0, fileid, recno));

	size += __wt_vsize_uint(size) - 1;
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, fileid, recno));

	logrec->size += size;
	return (0);
}

int
__wt_logop_col_remove_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
    uint32_t *fileidp, uint64_t *recnop)
{
	const char *fmt = WT_UNCHECKED_STRING(IIIr);
	uint32_t optype, size;

	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &optype, &size, fileidp, recnop));
	WT_ASSERT(session, optype == WT_LOGOP_COL_REMOVE);

	*pp += size;
	return (0);
}

int
__wt_logop_col_remove_print(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	uint32_t fileid;
	uint64_t recno;

	WT_RET(__wt_logop_col_remove_unpack(
	    session, pp, end, &fileid, &recno));

	fprintf(out, "\t" "fileid: %" PRIu32 "\n", fileid);
	fprintf(out, "\t" "recno: %" PRIu64 "\n", recno);
	return (0);
}

int
__wt_logop_row_put_pack(
    WT_SESSION_IMPL *session, WT_ITEM *logrec,
    uint32_t fileid, WT_ITEM *key, WT_ITEM *value)
{
	const char *fmt = WT_UNCHECKED_STRING(IIIuu);
	size_t size;
	uint32_t optype, recsize;

	optype = WT_LOGOP_ROW_PUT;
	WT_RET(__wt_struct_size(session, &size, fmt,
	    optype, 0, fileid, key, value));

	size += __wt_vsize_uint(size) - 1;
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, fileid, key, value));

	logrec->size += size;
	return (0);
}

int
__wt_logop_row_put_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
    uint32_t *fileidp, WT_ITEM *keyp, WT_ITEM *valuep)
{
	const char *fmt = WT_UNCHECKED_STRING(IIIuu);
	uint32_t optype, size;

	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &optype, &size, fileidp, keyp, valuep));
	WT_ASSERT(session, optype == WT_LOGOP_ROW_PUT);

	*pp += size;
	return (0);
}

int
__wt_logop_row_put_print(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	uint32_t fileid;
	WT_ITEM key;
	WT_ITEM value;

	WT_RET(__wt_logop_row_put_unpack(
	    session, pp, end, &fileid, &key, &value));

	fprintf(out, "\t" "fileid: %" PRIu32 "\n", fileid);
	fprintf(out, "\t" "key: %.*s\n",
	    (int)key.size, (const char *)key.data);
	fprintf(out, "\t" "value: %.*s\n",
	    (int)value.size, (const char *)value.data);
	return (0);
}

int
__wt_logop_row_remove_pack(
    WT_SESSION_IMPL *session, WT_ITEM *logrec,
    uint32_t fileid, WT_ITEM *key)
{
	const char *fmt = WT_UNCHECKED_STRING(IIIu);
	size_t size;
	uint32_t optype, recsize;

	optype = WT_LOGOP_ROW_REMOVE;
	WT_RET(__wt_struct_size(session, &size, fmt,
	    optype, 0, fileid, key));

	size += __wt_vsize_uint(size) - 1;
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, fileid, key));

	logrec->size += size;
	return (0);
}

int
__wt_logop_row_remove_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
    uint32_t *fileidp, WT_ITEM *keyp)
{
	const char *fmt = WT_UNCHECKED_STRING(IIIu);
	uint32_t optype, size;

	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &optype, &size, fileidp, keyp));
	WT_ASSERT(session, optype == WT_LOGOP_ROW_REMOVE);

	*pp += size;
	return (0);
}

int
__wt_logop_row_remove_print(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	uint32_t fileid;
	WT_ITEM key;

	WT_RET(__wt_logop_row_remove_unpack(
	    session, pp, end, &fileid, &key));

	fprintf(out, "\t" "fileid: %" PRIu32 "\n", fileid);
	fprintf(out, "\t" "key: %.*s\n",
	    (int)key.size, (const char *)key.data);
	return (0);
}
