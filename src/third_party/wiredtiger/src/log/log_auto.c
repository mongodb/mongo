/* DO NOT EDIT: automatically built by dist/log.py. */

#include "wt_internal.h"

int
__wt_logrec_alloc(WT_SESSION_IMPL *session, size_t size, WT_ITEM **logrecp)
{
    WT_ITEM *logrec;

    WT_RET(__wt_scr_alloc(session, WT_ALIGN(size + 1, WT_LOG_ALIGN), &logrec));
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
__wt_logrec_read(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, uint32_t *rectypep)
{
    uint64_t rectype;

    WT_UNUSED(session);
    WT_RET(__wt_vunpack_uint(pp, WT_PTRDIFF(end, *pp), &rectype));
    *rectypep = (uint32_t)rectype;
    return (0);
}

int
__wt_logop_read(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, uint32_t *optypep,
  uint32_t *opsizep)
{
    return (__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), "II", optypep, opsizep));
}

static int
__logrec_make_json_str(WT_SESSION_IMPL *session, WT_ITEM **escapedp, WT_ITEM *item)
{
    size_t needed;

    needed = (item->size * WT_MAX_JSON_ENCODE) + 1;

    if (*escapedp == NULL)
        WT_RET(__wt_scr_alloc(session, needed, escapedp));
    else
        WT_RET(__wt_buf_grow(session, *escapedp, needed));
    WT_IGNORE_RET(
      __wt_json_unpack_str((*escapedp)->mem, (*escapedp)->memsize, item->data, item->size));
    return (0);
}

static int
__logrec_make_hex_str(WT_SESSION_IMPL *session, WT_ITEM **escapedp, WT_ITEM *item)
{
    size_t needed;

    needed = (item->size * 2) + 1;

    if (*escapedp == NULL)
        WT_RET(__wt_scr_alloc(session, needed, escapedp));
    else
        WT_RET(__wt_buf_grow(session, *escapedp, needed));
    __wt_fill_hex(item->data, item->size, (*escapedp)->mem, (*escapedp)->memsize, NULL);
    return (0);
}

int
__wt_logop_col_modify_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, uint64_t recno, WT_ITEM *value)
{
    const char *fmt = WT_UNCHECKED_STRING(IIIru);
    size_t size;
    uint32_t optype, recsize;

    optype = WT_LOGOP_COL_MODIFY;
    WT_RET(__wt_struct_size(session, &size, fmt, optype, 0, fileid, recno, value));

    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
    recsize = (uint32_t)size;
    WT_RET(__wt_struct_pack(session, (uint8_t *)logrec->data + logrec->size, size, fmt, optype,
      recsize, fileid, recno, value));

    logrec->size += (uint32_t)size;
    return (0);
}

int
__wt_logop_col_modify_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, uint64_t *recnop, WT_ITEM *valuep)
{
    WT_DECL_RET;
    const char *fmt = WT_UNCHECKED_STRING(IIIru);
    uint32_t optype, size;

    if ((ret = __wt_struct_unpack(
           session, *pp, WT_PTRDIFF(end, *pp), fmt, &optype, &size, fileidp, recnop, valuep)) != 0)
        WT_RET_MSG(session, ret, "logop_col_modify: unpack failure");
    WT_ASSERT(session, optype == WT_LOGOP_COL_MODIFY);

    *pp += size;
    return (0);
}

int
__wt_logop_col_modify_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    WT_DECL_RET;
    uint32_t fileid;
    uint64_t recno;
    WT_ITEM value;
    WT_DECL_ITEM(escaped);

    WT_RET(__wt_logop_col_modify_unpack(session, pp, end, &fileid, &recno, &value));

    if (!FLD_ISSET(args->flags, WT_TXN_PRINTLOG_UNREDACT) && fileid != WT_METAFILE_ID)
        return (__wt_fprintf(session, args->fs, " REDACTED"));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"col_modify\",\n"));
    WT_ERR(__wt_fprintf(
      session, args->fs, "        \"fileid\": %" PRIu32 " 0x%" PRIx32 ",\n", fileid, fileid));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"recno\": %" PRIu64 ",\n", recno));
    WT_ERR(__logrec_make_json_str(session, &escaped, &value));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"value\": \"%s\"", (char *)escaped->mem));
    if (FLD_ISSET(args->flags, WT_TXN_PRINTLOG_HEX)) {
        WT_ERR(__logrec_make_hex_str(session, &escaped, &value));
        WT_ERR(__wt_fprintf(
          session, args->fs, ",\n        \"value-hex\": \"%s\"", (char *)escaped->mem));
    }

err:
    __wt_scr_free(session, &escaped);
    return (ret);
}

int
__wt_logop_col_put_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, uint64_t recno, WT_ITEM *value)
{
    const char *fmt = WT_UNCHECKED_STRING(IIIru);
    size_t size;
    uint32_t optype, recsize;

    optype = WT_LOGOP_COL_PUT;
    WT_RET(__wt_struct_size(session, &size, fmt, optype, 0, fileid, recno, value));

    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
    recsize = (uint32_t)size;
    WT_RET(__wt_struct_pack(session, (uint8_t *)logrec->data + logrec->size, size, fmt, optype,
      recsize, fileid, recno, value));

    logrec->size += (uint32_t)size;
    return (0);
}

int
__wt_logop_col_put_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, uint64_t *recnop, WT_ITEM *valuep)
{
    WT_DECL_RET;
    const char *fmt = WT_UNCHECKED_STRING(IIIru);
    uint32_t optype, size;

    if ((ret = __wt_struct_unpack(
           session, *pp, WT_PTRDIFF(end, *pp), fmt, &optype, &size, fileidp, recnop, valuep)) != 0)
        WT_RET_MSG(session, ret, "logop_col_put: unpack failure");
    WT_ASSERT(session, optype == WT_LOGOP_COL_PUT);

    *pp += size;
    return (0);
}

int
__wt_logop_col_put_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    WT_DECL_RET;
    uint32_t fileid;
    uint64_t recno;
    WT_ITEM value;
    WT_DECL_ITEM(escaped);

    WT_RET(__wt_logop_col_put_unpack(session, pp, end, &fileid, &recno, &value));

    if (!FLD_ISSET(args->flags, WT_TXN_PRINTLOG_UNREDACT) && fileid != WT_METAFILE_ID)
        return (__wt_fprintf(session, args->fs, " REDACTED"));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"col_put\",\n"));
    WT_ERR(__wt_fprintf(
      session, args->fs, "        \"fileid\": %" PRIu32 " 0x%" PRIx32 ",\n", fileid, fileid));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"recno\": %" PRIu64 ",\n", recno));
    WT_ERR(__logrec_make_json_str(session, &escaped, &value));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"value\": \"%s\"", (char *)escaped->mem));
    if (FLD_ISSET(args->flags, WT_TXN_PRINTLOG_HEX)) {
        WT_ERR(__logrec_make_hex_str(session, &escaped, &value));
        WT_ERR(__wt_fprintf(
          session, args->fs, ",\n        \"value-hex\": \"%s\"", (char *)escaped->mem));
    }

err:
    __wt_scr_free(session, &escaped);
    return (ret);
}

int
__wt_logop_col_remove_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, uint64_t recno)
{
    const char *fmt = WT_UNCHECKED_STRING(IIIr);
    size_t size;
    uint32_t optype, recsize;

    optype = WT_LOGOP_COL_REMOVE;
    WT_RET(__wt_struct_size(session, &size, fmt, optype, 0, fileid, recno));

    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
    recsize = (uint32_t)size;
    WT_RET(__wt_struct_pack(
      session, (uint8_t *)logrec->data + logrec->size, size, fmt, optype, recsize, fileid, recno));

    logrec->size += (uint32_t)size;
    return (0);
}

int
__wt_logop_col_remove_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, uint64_t *recnop)
{
    WT_DECL_RET;
    const char *fmt = WT_UNCHECKED_STRING(IIIr);
    uint32_t optype, size;

    if ((ret = __wt_struct_unpack(
           session, *pp, WT_PTRDIFF(end, *pp), fmt, &optype, &size, fileidp, recnop)) != 0)
        WT_RET_MSG(session, ret, "logop_col_remove: unpack failure");
    WT_ASSERT(session, optype == WT_LOGOP_COL_REMOVE);

    *pp += size;
    return (0);
}

int
__wt_logop_col_remove_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    uint32_t fileid;
    uint64_t recno;

    WT_RET(__wt_logop_col_remove_unpack(session, pp, end, &fileid, &recno));

    if (!FLD_ISSET(args->flags, WT_TXN_PRINTLOG_UNREDACT) && fileid != WT_METAFILE_ID)
        return (__wt_fprintf(session, args->fs, " REDACTED"));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"col_remove\",\n"));
    WT_RET(__wt_fprintf(
      session, args->fs, "        \"fileid\": %" PRIu32 " 0x%" PRIx32 ",\n", fileid, fileid));
    WT_RET(__wt_fprintf(session, args->fs, "        \"recno\": %" PRIu64 "", recno));
    return (0);
}

int
__wt_logop_col_truncate_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, uint64_t start, uint64_t stop)
{
    const char *fmt = WT_UNCHECKED_STRING(IIIrr);
    size_t size;
    uint32_t optype, recsize;

    optype = WT_LOGOP_COL_TRUNCATE;
    WT_RET(__wt_struct_size(session, &size, fmt, optype, 0, fileid, start, stop));

    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
    recsize = (uint32_t)size;
    WT_RET(__wt_struct_pack(session, (uint8_t *)logrec->data + logrec->size, size, fmt, optype,
      recsize, fileid, start, stop));

    logrec->size += (uint32_t)size;
    return (0);
}

int
__wt_logop_col_truncate_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, uint64_t *startp, uint64_t *stopp)
{
    WT_DECL_RET;
    const char *fmt = WT_UNCHECKED_STRING(IIIrr);
    uint32_t optype, size;

    if ((ret = __wt_struct_unpack(
           session, *pp, WT_PTRDIFF(end, *pp), fmt, &optype, &size, fileidp, startp, stopp)) != 0)
        WT_RET_MSG(session, ret, "logop_col_truncate: unpack failure");
    WT_ASSERT(session, optype == WT_LOGOP_COL_TRUNCATE);

    *pp += size;
    return (0);
}

int
__wt_logop_col_truncate_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    uint32_t fileid;
    uint64_t start;
    uint64_t stop;

    WT_RET(__wt_logop_col_truncate_unpack(session, pp, end, &fileid, &start, &stop));

    if (!FLD_ISSET(args->flags, WT_TXN_PRINTLOG_UNREDACT) && fileid != WT_METAFILE_ID)
        return (__wt_fprintf(session, args->fs, " REDACTED"));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"col_truncate\",\n"));
    WT_RET(__wt_fprintf(
      session, args->fs, "        \"fileid\": %" PRIu32 " 0x%" PRIx32 ",\n", fileid, fileid));
    WT_RET(__wt_fprintf(session, args->fs, "        \"start\": %" PRIu64 ",\n", start));
    WT_RET(__wt_fprintf(session, args->fs, "        \"stop\": %" PRIu64 "", stop));
    return (0);
}

int
__wt_logop_row_modify_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, WT_ITEM *key, WT_ITEM *value)
{
    const char *fmt = WT_UNCHECKED_STRING(IIIuu);
    size_t size;
    uint32_t optype, recsize;

    optype = WT_LOGOP_ROW_MODIFY;
    WT_RET(__wt_struct_size(session, &size, fmt, optype, 0, fileid, key, value));

    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
    recsize = (uint32_t)size;
    WT_RET(__wt_struct_pack(session, (uint8_t *)logrec->data + logrec->size, size, fmt, optype,
      recsize, fileid, key, value));

    logrec->size += (uint32_t)size;
    return (0);
}

int
__wt_logop_row_modify_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, WT_ITEM *keyp, WT_ITEM *valuep)
{
    WT_DECL_RET;
    const char *fmt = WT_UNCHECKED_STRING(IIIuu);
    uint32_t optype, size;

    if ((ret = __wt_struct_unpack(
           session, *pp, WT_PTRDIFF(end, *pp), fmt, &optype, &size, fileidp, keyp, valuep)) != 0)
        WT_RET_MSG(session, ret, "logop_row_modify: unpack failure");
    WT_ASSERT(session, optype == WT_LOGOP_ROW_MODIFY);

    *pp += size;
    return (0);
}

int
__wt_logop_row_modify_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    WT_DECL_RET;
    uint32_t fileid;
    WT_ITEM key;
    WT_ITEM value;
    WT_DECL_ITEM(escaped);

    WT_RET(__wt_logop_row_modify_unpack(session, pp, end, &fileid, &key, &value));

    if (!FLD_ISSET(args->flags, WT_TXN_PRINTLOG_UNREDACT) && fileid != WT_METAFILE_ID)
        return (__wt_fprintf(session, args->fs, " REDACTED"));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"row_modify\",\n"));
    WT_ERR(__wt_fprintf(
      session, args->fs, "        \"fileid\": %" PRIu32 " 0x%" PRIx32 ",\n", fileid, fileid));
    WT_ERR(__logrec_make_json_str(session, &escaped, &key));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"key\": \"%s\",\n", (char *)escaped->mem));
    if (FLD_ISSET(args->flags, WT_TXN_PRINTLOG_HEX)) {
        WT_ERR(__logrec_make_hex_str(session, &escaped, &key));
        WT_ERR(
          __wt_fprintf(session, args->fs, "        \"key-hex\": \"%s\",\n", (char *)escaped->mem));
    }
    WT_ERR(__logrec_make_json_str(session, &escaped, &value));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"value\": \"%s\"", (char *)escaped->mem));
    if (FLD_ISSET(args->flags, WT_TXN_PRINTLOG_HEX)) {
        WT_ERR(__logrec_make_hex_str(session, &escaped, &value));
        WT_ERR(__wt_fprintf(
          session, args->fs, ",\n        \"value-hex\": \"%s\"", (char *)escaped->mem));
    }

err:
    __wt_scr_free(session, &escaped);
    return (ret);
}

int
__wt_logop_row_put_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, WT_ITEM *key, WT_ITEM *value)
{
    const char *fmt = WT_UNCHECKED_STRING(IIIuu);
    size_t size;
    uint32_t optype, recsize;

    optype = WT_LOGOP_ROW_PUT;
    WT_RET(__wt_struct_size(session, &size, fmt, optype, 0, fileid, key, value));

    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
    recsize = (uint32_t)size;
    WT_RET(__wt_struct_pack(session, (uint8_t *)logrec->data + logrec->size, size, fmt, optype,
      recsize, fileid, key, value));

    logrec->size += (uint32_t)size;
    return (0);
}

int
__wt_logop_row_put_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, WT_ITEM *keyp, WT_ITEM *valuep)
{
    WT_DECL_RET;
    const char *fmt = WT_UNCHECKED_STRING(IIIuu);
    uint32_t optype, size;

    if ((ret = __wt_struct_unpack(
           session, *pp, WT_PTRDIFF(end, *pp), fmt, &optype, &size, fileidp, keyp, valuep)) != 0)
        WT_RET_MSG(session, ret, "logop_row_put: unpack failure");
    WT_ASSERT(session, optype == WT_LOGOP_ROW_PUT);

    *pp += size;
    return (0);
}

int
__wt_logop_row_put_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    WT_DECL_RET;
    uint32_t fileid;
    WT_ITEM key;
    WT_ITEM value;
    WT_DECL_ITEM(escaped);

    WT_RET(__wt_logop_row_put_unpack(session, pp, end, &fileid, &key, &value));

    if (!FLD_ISSET(args->flags, WT_TXN_PRINTLOG_UNREDACT) && fileid != WT_METAFILE_ID)
        return (__wt_fprintf(session, args->fs, " REDACTED"));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"row_put\",\n"));
    WT_ERR(__wt_fprintf(
      session, args->fs, "        \"fileid\": %" PRIu32 " 0x%" PRIx32 ",\n", fileid, fileid));
    WT_ERR(__logrec_make_json_str(session, &escaped, &key));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"key\": \"%s\",\n", (char *)escaped->mem));
    if (FLD_ISSET(args->flags, WT_TXN_PRINTLOG_HEX)) {
        WT_ERR(__logrec_make_hex_str(session, &escaped, &key));
        WT_ERR(
          __wt_fprintf(session, args->fs, "        \"key-hex\": \"%s\",\n", (char *)escaped->mem));
    }
    WT_ERR(__logrec_make_json_str(session, &escaped, &value));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"value\": \"%s\"", (char *)escaped->mem));
    if (FLD_ISSET(args->flags, WT_TXN_PRINTLOG_HEX)) {
        WT_ERR(__logrec_make_hex_str(session, &escaped, &value));
        WT_ERR(__wt_fprintf(
          session, args->fs, ",\n        \"value-hex\": \"%s\"", (char *)escaped->mem));
    }

err:
    __wt_scr_free(session, &escaped);
    return (ret);
}

int
__wt_logop_row_remove_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, WT_ITEM *key)
{
    const char *fmt = WT_UNCHECKED_STRING(IIIu);
    size_t size;
    uint32_t optype, recsize;

    optype = WT_LOGOP_ROW_REMOVE;
    WT_RET(__wt_struct_size(session, &size, fmt, optype, 0, fileid, key));

    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
    recsize = (uint32_t)size;
    WT_RET(__wt_struct_pack(
      session, (uint8_t *)logrec->data + logrec->size, size, fmt, optype, recsize, fileid, key));

    logrec->size += (uint32_t)size;
    return (0);
}

int
__wt_logop_row_remove_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, WT_ITEM *keyp)
{
    WT_DECL_RET;
    const char *fmt = WT_UNCHECKED_STRING(IIIu);
    uint32_t optype, size;

    if ((ret = __wt_struct_unpack(
           session, *pp, WT_PTRDIFF(end, *pp), fmt, &optype, &size, fileidp, keyp)) != 0)
        WT_RET_MSG(session, ret, "logop_row_remove: unpack failure");
    WT_ASSERT(session, optype == WT_LOGOP_ROW_REMOVE);

    *pp += size;
    return (0);
}

int
__wt_logop_row_remove_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    WT_DECL_RET;
    uint32_t fileid;
    WT_ITEM key;
    WT_DECL_ITEM(escaped);

    WT_RET(__wt_logop_row_remove_unpack(session, pp, end, &fileid, &key));

    if (!FLD_ISSET(args->flags, WT_TXN_PRINTLOG_UNREDACT) && fileid != WT_METAFILE_ID)
        return (__wt_fprintf(session, args->fs, " REDACTED"));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"row_remove\",\n"));
    WT_ERR(__wt_fprintf(
      session, args->fs, "        \"fileid\": %" PRIu32 " 0x%" PRIx32 ",\n", fileid, fileid));
    WT_ERR(__logrec_make_json_str(session, &escaped, &key));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"key\": \"%s\"", (char *)escaped->mem));
    if (FLD_ISSET(args->flags, WT_TXN_PRINTLOG_HEX)) {
        WT_ERR(__logrec_make_hex_str(session, &escaped, &key));
        WT_ERR(
          __wt_fprintf(session, args->fs, ",\n        \"key-hex\": \"%s\"", (char *)escaped->mem));
    }

err:
    __wt_scr_free(session, &escaped);
    return (ret);
}

int
__wt_logop_row_truncate_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  WT_ITEM *start, WT_ITEM *stop, uint32_t mode)
{
    const char *fmt = WT_UNCHECKED_STRING(IIIuuI);
    size_t size;
    uint32_t optype, recsize;

    optype = WT_LOGOP_ROW_TRUNCATE;
    WT_RET(__wt_struct_size(session, &size, fmt, optype, 0, fileid, start, stop, mode));

    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
    recsize = (uint32_t)size;
    WT_RET(__wt_struct_pack(session, (uint8_t *)logrec->data + logrec->size, size, fmt, optype,
      recsize, fileid, start, stop, mode));

    logrec->size += (uint32_t)size;
    return (0);
}

int
__wt_logop_row_truncate_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, WT_ITEM *startp, WT_ITEM *stopp, uint32_t *modep)
{
    WT_DECL_RET;
    const char *fmt = WT_UNCHECKED_STRING(IIIuuI);
    uint32_t optype, size;

    if ((ret = __wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt, &optype, &size, fileidp,
           startp, stopp, modep)) != 0)
        WT_RET_MSG(session, ret, "logop_row_truncate: unpack failure");
    WT_ASSERT(session, optype == WT_LOGOP_ROW_TRUNCATE);

    *pp += size;
    return (0);
}

int
__wt_logop_row_truncate_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    WT_DECL_RET;
    uint32_t fileid;
    WT_ITEM start;
    WT_ITEM stop;
    uint32_t mode;
    WT_DECL_ITEM(escaped);

    WT_RET(__wt_logop_row_truncate_unpack(session, pp, end, &fileid, &start, &stop, &mode));

    if (!FLD_ISSET(args->flags, WT_TXN_PRINTLOG_UNREDACT) && fileid != WT_METAFILE_ID)
        return (__wt_fprintf(session, args->fs, " REDACTED"));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"row_truncate\",\n"));
    WT_ERR(__wt_fprintf(
      session, args->fs, "        \"fileid\": %" PRIu32 " 0x%" PRIx32 ",\n", fileid, fileid));
    WT_ERR(__logrec_make_json_str(session, &escaped, &start));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"start\": \"%s\",\n", (char *)escaped->mem));
    if (FLD_ISSET(args->flags, WT_TXN_PRINTLOG_HEX)) {
        WT_ERR(__logrec_make_hex_str(session, &escaped, &start));
        WT_ERR(__wt_fprintf(
          session, args->fs, "        \"start-hex\": \"%s\",\n", (char *)escaped->mem));
    }
    WT_ERR(__logrec_make_json_str(session, &escaped, &stop));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"stop\": \"%s\",\n", (char *)escaped->mem));
    if (FLD_ISSET(args->flags, WT_TXN_PRINTLOG_HEX)) {
        WT_ERR(__logrec_make_hex_str(session, &escaped, &stop));
        WT_ERR(
          __wt_fprintf(session, args->fs, "        \"stop-hex\": \"%s\",\n", (char *)escaped->mem));
    }
    WT_ERR(__wt_fprintf(session, args->fs, "        \"mode\": %" PRIu32 "", mode));

err:
    __wt_scr_free(session, &escaped);
    return (ret);
}

int
__wt_logop_checkpoint_start_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec)
{
    const char *fmt = WT_UNCHECKED_STRING(II);
    size_t size;
    uint32_t optype, recsize;

    optype = WT_LOGOP_CHECKPOINT_START;
    WT_RET(__wt_struct_size(session, &size, fmt, optype, 0));

    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
    recsize = (uint32_t)size;
    WT_RET(__wt_struct_pack(
      session, (uint8_t *)logrec->data + logrec->size, size, fmt, optype, recsize));

    logrec->size += (uint32_t)size;
    return (0);
}

int
__wt_logop_checkpoint_start_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end)
{
    WT_DECL_RET;
    const char *fmt = WT_UNCHECKED_STRING(II);
    uint32_t optype, size;

    if ((ret = __wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt, &optype, &size)) != 0)
        WT_RET_MSG(session, ret, "logop_checkpoint_start: unpack failure");
    WT_ASSERT(session, optype == WT_LOGOP_CHECKPOINT_START);

    *pp += size;
    return (0);
}

int
__wt_logop_checkpoint_start_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{

    WT_RET(__wt_logop_checkpoint_start_unpack(session, pp, end));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"checkpoint_start\",\n"));

    return (0);
}

int
__wt_logop_prev_lsn_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *prev_lsn)
{
    const char *fmt = WT_UNCHECKED_STRING(IIII);
    size_t size;
    uint32_t optype, recsize;

    optype = WT_LOGOP_PREV_LSN;
    WT_RET(__wt_struct_size(session, &size, fmt, optype, 0, prev_lsn->l.file, prev_lsn->l.offset));

    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
    recsize = (uint32_t)size;
    WT_RET(__wt_struct_pack(session, (uint8_t *)logrec->data + logrec->size, size, fmt, optype,
      recsize, prev_lsn->l.file, prev_lsn->l.offset));

    logrec->size += (uint32_t)size;
    return (0);
}

int
__wt_logop_prev_lsn_unpack(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_LSN *prev_lsnp)
{
    WT_DECL_RET;
    const char *fmt = WT_UNCHECKED_STRING(IIII);
    uint32_t optype, size;

    if ((ret = __wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt, &optype, &size,
           &prev_lsnp->l.file, &prev_lsnp->l.offset)) != 0)
        WT_RET_MSG(session, ret, "logop_prev_lsn: unpack failure");
    WT_ASSERT(session, optype == WT_LOGOP_PREV_LSN);

    *pp += size;
    return (0);
}

int
__wt_logop_prev_lsn_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    WT_LSN prev_lsn;

    WT_RET(__wt_logop_prev_lsn_unpack(session, pp, end, &prev_lsn));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"prev_lsn\",\n"));
    WT_RET(__wt_fprintf(session, args->fs, "        \"prev_lsn\": [%" PRIu32 ", %" PRIu32 "]",
      prev_lsn.l.file, prev_lsn.l.offset));
    return (0);
}

int
__wt_logop_txn_timestamp_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint64_t time_sec,
  uint64_t time_nsec, uint64_t commit_ts, uint64_t durable_ts, uint64_t first_commit_ts,
  uint64_t prepare_ts, uint64_t read_ts)
{
    const char *fmt = WT_UNCHECKED_STRING(IIQQQQQQQ);
    size_t size;
    uint32_t optype, recsize;

    optype = WT_LOGOP_TXN_TIMESTAMP;
    WT_RET(__wt_struct_size(session, &size, fmt, optype, 0, time_sec, time_nsec, commit_ts,
      durable_ts, first_commit_ts, prepare_ts, read_ts));

    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
    recsize = (uint32_t)size;
    WT_RET(__wt_struct_pack(session, (uint8_t *)logrec->data + logrec->size, size, fmt, optype,
      recsize, time_sec, time_nsec, commit_ts, durable_ts, first_commit_ts, prepare_ts, read_ts));

    logrec->size += (uint32_t)size;
    return (0);
}

int
__wt_logop_txn_timestamp_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint64_t *time_secp, uint64_t *time_nsecp, uint64_t *commit_tsp, uint64_t *durable_tsp,
  uint64_t *first_commit_tsp, uint64_t *prepare_tsp, uint64_t *read_tsp)
{
    WT_DECL_RET;
    const char *fmt = WT_UNCHECKED_STRING(IIQQQQQQQ);
    uint32_t optype, size;

    if ((ret =
            __wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt, &optype, &size, time_secp,
              time_nsecp, commit_tsp, durable_tsp, first_commit_tsp, prepare_tsp, read_tsp)) != 0)
        WT_RET_MSG(session, ret, "logop_txn_timestamp: unpack failure");
    WT_ASSERT(session, optype == WT_LOGOP_TXN_TIMESTAMP);

    *pp += size;
    return (0);
}

int
__wt_logop_txn_timestamp_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    uint64_t time_sec;
    uint64_t time_nsec;
    uint64_t commit_ts;
    uint64_t durable_ts;
    uint64_t first_commit_ts;
    uint64_t prepare_ts;
    uint64_t read_ts;

    WT_RET(__wt_logop_txn_timestamp_unpack(session, pp, end, &time_sec, &time_nsec, &commit_ts,
      &durable_ts, &first_commit_ts, &prepare_ts, &read_ts));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"txn_timestamp\",\n"));
    WT_RET(__wt_fprintf(session, args->fs, "        \"time_sec\": %" PRIu64 ",\n", time_sec));
    WT_RET(__wt_fprintf(session, args->fs, "        \"time_nsec\": %" PRIu64 ",\n", time_nsec));
    WT_RET(__wt_fprintf(session, args->fs, "        \"commit_ts\": %" PRIu64 ",\n", commit_ts));
    WT_RET(__wt_fprintf(session, args->fs, "        \"durable_ts\": %" PRIu64 ",\n", durable_ts));
    WT_RET(__wt_fprintf(
      session, args->fs, "        \"first_commit_ts\": %" PRIu64 ",\n", first_commit_ts));
    WT_RET(__wt_fprintf(session, args->fs, "        \"prepare_ts\": %" PRIu64 ",\n", prepare_ts));
    WT_RET(__wt_fprintf(session, args->fs, "        \"read_ts\": %" PRIu64 "", read_ts));
    return (0);
}

int
__wt_txn_op_printlog(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    uint32_t optype, opsize;

    /* Peek at the size and the type. */
    WT_RET(__wt_logop_read(session, pp, end, &optype, &opsize));
    end = *pp + opsize;

    switch (optype) {
    case WT_LOGOP_COL_MODIFY:
        WT_RET(__wt_logop_col_modify_print(session, pp, end, args));
        break;

    case WT_LOGOP_COL_PUT:
        WT_RET(__wt_logop_col_put_print(session, pp, end, args));
        break;

    case WT_LOGOP_COL_REMOVE:
        WT_RET(__wt_logop_col_remove_print(session, pp, end, args));
        break;

    case WT_LOGOP_COL_TRUNCATE:
        WT_RET(__wt_logop_col_truncate_print(session, pp, end, args));
        break;

    case WT_LOGOP_ROW_MODIFY:
        WT_RET(__wt_logop_row_modify_print(session, pp, end, args));
        break;

    case WT_LOGOP_ROW_PUT:
        WT_RET(__wt_logop_row_put_print(session, pp, end, args));
        break;

    case WT_LOGOP_ROW_REMOVE:
        WT_RET(__wt_logop_row_remove_print(session, pp, end, args));
        break;

    case WT_LOGOP_ROW_TRUNCATE:
        WT_RET(__wt_logop_row_truncate_print(session, pp, end, args));
        break;

    case WT_LOGOP_CHECKPOINT_START:
        WT_RET(__wt_logop_checkpoint_start_print(session, pp, end, args));
        break;

    case WT_LOGOP_PREV_LSN:
        WT_RET(__wt_logop_prev_lsn_print(session, pp, end, args));
        break;

    case WT_LOGOP_TXN_TIMESTAMP:
        WT_RET(__wt_logop_txn_timestamp_print(session, pp, end, args));
        break;

    default:
        return (__wt_illegal_value(session, optype));
    }

    return (0);
}
