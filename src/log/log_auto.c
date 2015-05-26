/* DO NOT EDIT: automatically built by dist/log.py. */

#include "wt_internal.h"

int
__wt_logrec_alloc(WT_SESSION_IMPL *session, size_t size, WT_ITEM **logrecp)
{
	WT_ITEM *logrec;

	WT_RET(
	    __wt_scr_alloc(session, WT_ALIGN(size + 1, WT_LOG_ALIGN), &logrec));
	WT_CLEAR(*(WT_LOG_RECORD *)logrec->data);
	logrec->size = offsetof(WT_LOG_RECORD, record);

	*logrecp = logrec;
	return (0);
}

void
__wt_logrec_free(WT_SESSION_IMPL *session, WT_ITEM **logrecp)
{
	__wt_scr_free(session, logrecp);
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

static size_t
__logrec_json_unpack_str(char *dest, size_t destlen, const char *src,
    size_t srclen)
{
	size_t total;
	size_t n;

	total = 0;
	while (srclen > 0) {
		n = __wt_json_unpack_char(*src++, (u_char *)dest, destlen, 0);
		srclen--;
		if (n > destlen)
			destlen = 0;
		else {
			destlen -= n;
			dest += n;
		}
		total += n;
	}
	if (destlen > 0)
		*dest = '\0';
	return (total + 1);
}

static int
__logrec_jsonify_str(WT_SESSION_IMPL *session, char **destp, WT_ITEM *item)
{
	size_t needed;

	needed = __logrec_json_unpack_str(NULL, 0, item->data, item->size);
	WT_RET(__wt_realloc(session, NULL, needed, destp));
	(void)__logrec_json_unpack_str(*destp, needed, item->data, item->size);
	return (0);
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

	__wt_struct_size_adjust(session, &size);
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, fileid, recno, value));

	logrec->size += (uint32_t)size;
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
	WT_DECL_RET;
	uint32_t fileid;
	uint64_t recno;
	WT_ITEM value;
	char *escaped;

	escaped = NULL;
	WT_RET(__wt_logop_col_put_unpack(
	    session, pp, end, &fileid, &recno, &value));

	WT_RET(__wt_fprintf(out, " \"optype\": \"col_put\",\n"));
	WT_ERR(__wt_fprintf(out,
	    "        \"fileid\": \"%" PRIu32 "\",\n", fileid));
	WT_ERR(__wt_fprintf(out,
	    "        \"recno\": \"%" PRIu64 "\",\n", recno));
	WT_ERR(__logrec_jsonify_str(session, &escaped, &value));
	WT_ERR(__wt_fprintf(out,
	    "        \"value\": \"%s\"", escaped));

err:	__wt_free(session, escaped);
	return (ret);
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

	__wt_struct_size_adjust(session, &size);
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, fileid, recno));

	logrec->size += (uint32_t)size;
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

	WT_RET(__wt_fprintf(out, " \"optype\": \"col_remove\",\n"));
	WT_RET(__wt_fprintf(out,
	    "        \"fileid\": \"%" PRIu32 "\",\n", fileid));
	WT_RET(__wt_fprintf(out,
	    "        \"recno\": \"%" PRIu64 "\"", recno));
	return (0);
}

int
__wt_logop_col_truncate_pack(
    WT_SESSION_IMPL *session, WT_ITEM *logrec,
    uint32_t fileid, uint64_t start, uint64_t stop)
{
	const char *fmt = WT_UNCHECKED_STRING(IIIrr);
	size_t size;
	uint32_t optype, recsize;

	optype = WT_LOGOP_COL_TRUNCATE;
	WT_RET(__wt_struct_size(session, &size, fmt,
	    optype, 0, fileid, start, stop));

	__wt_struct_size_adjust(session, &size);
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, fileid, start, stop));

	logrec->size += (uint32_t)size;
	return (0);
}

int
__wt_logop_col_truncate_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
    uint32_t *fileidp, uint64_t *startp, uint64_t *stopp)
{
	const char *fmt = WT_UNCHECKED_STRING(IIIrr);
	uint32_t optype, size;

	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &optype, &size, fileidp, startp, stopp));
	WT_ASSERT(session, optype == WT_LOGOP_COL_TRUNCATE);

	*pp += size;
	return (0);
}

int
__wt_logop_col_truncate_print(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	uint32_t fileid;
	uint64_t start;
	uint64_t stop;

	WT_RET(__wt_logop_col_truncate_unpack(
	    session, pp, end, &fileid, &start, &stop));

	WT_RET(__wt_fprintf(out, " \"optype\": \"col_truncate\",\n"));
	WT_RET(__wt_fprintf(out,
	    "        \"fileid\": \"%" PRIu32 "\",\n", fileid));
	WT_RET(__wt_fprintf(out,
	    "        \"start\": \"%" PRIu64 "\",\n", start));
	WT_RET(__wt_fprintf(out,
	    "        \"stop\": \"%" PRIu64 "\"", stop));
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

	__wt_struct_size_adjust(session, &size);
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, fileid, key, value));

	logrec->size += (uint32_t)size;
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
	WT_DECL_RET;
	uint32_t fileid;
	WT_ITEM key;
	WT_ITEM value;
	char *escaped;

	escaped = NULL;
	WT_RET(__wt_logop_row_put_unpack(
	    session, pp, end, &fileid, &key, &value));

	WT_RET(__wt_fprintf(out, " \"optype\": \"row_put\",\n"));
	WT_ERR(__wt_fprintf(out,
	    "        \"fileid\": \"%" PRIu32 "\",\n", fileid));
	WT_ERR(__logrec_jsonify_str(session, &escaped, &key));
	WT_ERR(__wt_fprintf(out,
	    "        \"key\": \"%s\",\n", escaped));
	WT_ERR(__logrec_jsonify_str(session, &escaped, &value));
	WT_ERR(__wt_fprintf(out,
	    "        \"value\": \"%s\"", escaped));

err:	__wt_free(session, escaped);
	return (ret);
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

	__wt_struct_size_adjust(session, &size);
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, fileid, key));

	logrec->size += (uint32_t)size;
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
	WT_DECL_RET;
	uint32_t fileid;
	WT_ITEM key;
	char *escaped;

	escaped = NULL;
	WT_RET(__wt_logop_row_remove_unpack(
	    session, pp, end, &fileid, &key));

	WT_RET(__wt_fprintf(out, " \"optype\": \"row_remove\",\n"));
	WT_ERR(__wt_fprintf(out,
	    "        \"fileid\": \"%" PRIu32 "\",\n", fileid));
	WT_ERR(__logrec_jsonify_str(session, &escaped, &key));
	WT_ERR(__wt_fprintf(out,
	    "        \"key\": \"%s\"", escaped));

err:	__wt_free(session, escaped);
	return (ret);
}

int
__wt_logop_row_truncate_pack(
    WT_SESSION_IMPL *session, WT_ITEM *logrec,
    uint32_t fileid, WT_ITEM *start, WT_ITEM *stop, uint32_t mode)
{
	const char *fmt = WT_UNCHECKED_STRING(IIIuuI);
	size_t size;
	uint32_t optype, recsize;

	optype = WT_LOGOP_ROW_TRUNCATE;
	WT_RET(__wt_struct_size(session, &size, fmt,
	    optype, 0, fileid, start, stop, mode));

	__wt_struct_size_adjust(session, &size);
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize, fileid, start, stop, mode));

	logrec->size += (uint32_t)size;
	return (0);
}

int
__wt_logop_row_truncate_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
    uint32_t *fileidp, WT_ITEM *startp, WT_ITEM *stopp, uint32_t *modep)
{
	const char *fmt = WT_UNCHECKED_STRING(IIIuuI);
	uint32_t optype, size;

	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &optype, &size, fileidp, startp, stopp, modep));
	WT_ASSERT(session, optype == WT_LOGOP_ROW_TRUNCATE);

	*pp += size;
	return (0);
}

int
__wt_logop_row_truncate_print(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	WT_DECL_RET;
	uint32_t fileid;
	WT_ITEM start;
	WT_ITEM stop;
	uint32_t mode;
	char *escaped;

	escaped = NULL;
	WT_RET(__wt_logop_row_truncate_unpack(
	    session, pp, end, &fileid, &start, &stop, &mode));

	WT_RET(__wt_fprintf(out, " \"optype\": \"row_truncate\",\n"));
	WT_ERR(__wt_fprintf(out,
	    "        \"fileid\": \"%" PRIu32 "\",\n", fileid));
	WT_ERR(__logrec_jsonify_str(session, &escaped, &start));
	WT_ERR(__wt_fprintf(out,
	    "        \"start\": \"%s\",\n", escaped));
	WT_ERR(__logrec_jsonify_str(session, &escaped, &stop));
	WT_ERR(__wt_fprintf(out,
	    "        \"stop\": \"%s\",\n", escaped));
	WT_ERR(__wt_fprintf(out,
	    "        \"mode\": \"%" PRIu32 "\"", mode));

err:	__wt_free(session, escaped);
	return (ret);
}

int
__wt_txn_op_printlog(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	uint32_t optype, opsize;

	/* Peek at the size and the type. */
	WT_RET(__wt_logop_read(session, pp, end, &optype, &opsize));
	end = *pp + opsize;

	switch (optype) {
	case WT_LOGOP_COL_PUT:
		WT_RET(__wt_logop_col_put_print(session, pp, end, out));
		break;

	case WT_LOGOP_COL_REMOVE:
		WT_RET(__wt_logop_col_remove_print(session, pp, end, out));
		break;

	case WT_LOGOP_COL_TRUNCATE:
		WT_RET(__wt_logop_col_truncate_print(session, pp, end, out));
		break;

	case WT_LOGOP_ROW_PUT:
		WT_RET(__wt_logop_row_put_print(session, pp, end, out));
		break;

	case WT_LOGOP_ROW_REMOVE:
		WT_RET(__wt_logop_row_remove_print(session, pp, end, out));
		break;

	case WT_LOGOP_ROW_TRUNCATE:
		WT_RET(__wt_logop_row_truncate_print(session, pp, end, out));
		break;

	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}
