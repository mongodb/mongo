/* DO NOT EDIT: automatically built by dist/log.py. */

#include "wt_internal.h"
#include "log_private.h"

#define WT_SIZE_CHECK_PACK_PTR(p, end) WT_RET_TEST(!(p) || !(end) || (p) >= (end), ENOMEM)
#define WT_SIZE_CHECK_UNPACK_PTR(p, end) WT_RET_TEST(!(p) || !(end) || (p) >= (end), EINVAL)
#define WT_SIZE_CHECK_UNPACK_PTR0(p, end) WT_RET_TEST(!(p) || !(end) || (p) > (end), EINVAL)

/*
 * Defining PACKING_COMPATIBILITY_MODE makes __wt_logop_*_unpack functions behave in a more
 * compatible way with older versions of WiredTiger and wiredtiger_struct_unpack(...fmt...)
 * function. This only alters the behavior for corrupted binary data, returning some value rather
 * than failing with EINVAL.
 */

#ifndef PACKING_COMPATIBILITY_MODE
#define WT_CHECK_OPTYPE(session, opvar, op) \
    if (opvar != op)                        \
        WT_RET_MSG(session, EINVAL, "unpacking " #op ": optype mismatch");
#else
#define WT_CHECK_OPTYPE(session, opvar, op)
#endif

/*
 * __pack_encode_uintAny --
 *     Pack an unsigned integer.
 */
static WT_INLINE int
__pack_encode_uintAny(uint8_t **pp, uint8_t *end, uint64_t item)
{
    /* Check that there is at least one byte available:
     * the low-level routines treat zero length as unchecked. */
    WT_SIZE_CHECK_PACK_PTR(*pp, end);
    return (__wt_vpack_uint(pp, WT_PTRDIFF(end, *pp), item));
}

/*
 * __pack_encode_WT_ITEM --
 *     Pack a WT_ITEM structure - size and WT_ITEM.
 */
static WT_INLINE int
__pack_encode_WT_ITEM(uint8_t **pp, uint8_t *end, WT_ITEM *item)
{
    WT_RET(__wt_vpack_uint(pp, WT_PTRDIFF(end, *pp), item->size));
    if (item->size != 0) {
        WT_SIZE_CHECK_PACK(item->size, WT_PTRDIFF(end, *pp));
        memcpy(*pp, item->data, item->size);
        *pp += item->size;
    }
    return (0);
}

/*
 * __pack_encode_WT_ITEM_last --
 *     Pack a WT_ITEM structure without its size.
 */
static WT_INLINE int
__pack_encode_WT_ITEM_last(uint8_t **pp, uint8_t *end, WT_ITEM *item)
{
    if (item->size != 0) {
        WT_SIZE_CHECK_PACK(item->size, WT_PTRDIFF(end, *pp));
        memcpy(*pp, item->data, item->size);
        *pp += item->size;
    }
    return (0);
}

/*
 * __pack_encode_string --
 *     Pack a string.
 */
static WT_INLINE int
__pack_encode_string(uint8_t **pp, uint8_t *end, const char *item)
{
    size_t s, sz;

    sz = WT_PTRDIFF(end, *pp);
    s = __wt_strnlen(item, sz - 1);
    WT_SIZE_CHECK_PACK(s + 1, sz);
    memcpy(*pp, item, s);
    *pp += s;
    **pp = '\0';
    *pp += 1;
    return (0);
}

#define __pack_decode_uintAny(pp, end, TYPE, pval)                                             \
    do {                                                                                       \
        uint64_t v; /* Check that there is at least one byte available: the low-level routines \
                       treat zero length as unchecked. */                                      \
        WT_SIZE_CHECK_UNPACK_PTR(*pp, end);                                                    \
        WT_RET(__wt_vunpack_uint(pp, WT_PTRDIFF(end, *pp), &v));                               \
        *(pval) = (TYPE)v;                                                                     \
    } while (0)

#define __pack_decode_WT_ITEM(pp, end, val)                    \
    do {                                                       \
        __pack_decode_uintAny(pp, end, size_t, &val->size);    \
        WT_SIZE_CHECK_UNPACK(val->size, WT_PTRDIFF(end, *pp)); \
        val->data = *pp;                                       \
        *pp += val->size;                                      \
    } while (0)

#define __pack_decode_WT_ITEM_last(pp, end, val) \
    do {                                         \
        WT_SIZE_CHECK_UNPACK_PTR0(*pp, end);     \
        val->size = WT_PTRDIFF(end, *pp);        \
        val->data = *pp;                         \
        *pp += val->size;                        \
    } while (0)

#define __pack_decode_string(pp, end, val)             \
    do {                                               \
        size_t s;                                      \
        *val = (const char *)*pp;                      \
        s = strlen((const char *)*pp) + 1;             \
        WT_SIZE_CHECK_UNPACK(s, WT_PTRDIFF(end, *pp)); \
        *pp += s;                                      \
    } while (0)

/*
 * __wt_logrec_alloc --
 *     Allocate a new WT_ITEM structure.
 */
int
__wt_logrec_alloc(WT_SESSION_IMPL *session, size_t size, WT_ITEM **logrecp)
{
    WT_ITEM *logrec;

    WT_RET(__wt_scr_alloc(session, WT_ALIGN(size + 1, WTI_LOG_ALIGN), &logrec));
    WT_CLEAR(*(WT_LOG_RECORD *)logrec->data);
    logrec->size = offsetof(WT_LOG_RECORD, record);

    *logrecp = logrec;
    return (0);
}

/*
 * __wt_logrec_free --
 *     Free the given WT_ITEM structure.
 */
void
__wt_logrec_free(WT_SESSION_IMPL *session, WT_ITEM **logrecp)
{
    __wt_scr_free(session, logrecp);
}

/*
 * __wt_logrec_read --
 *     Read the record type.
 */
int
__wt_logrec_read(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, uint32_t *rectypep)
{
    WT_UNUSED(session);
    __pack_decode_uintAny(pp, end, uint32_t, rectypep);
    return (0);
}

/*
 * __wt_logop_read --
 *     Peek at the operation type.
 */
int
__wt_logop_read(WT_SESSION_IMPL *session, const uint8_t **pp_peek, const uint8_t *end,
  uint32_t *optypep, uint32_t *opsizep)
{
    const uint8_t *p, **pp;
    WT_UNUSED(session);

    p = *pp_peek;
    pp = &p;
    __pack_decode_uintAny(pp, end, uint32_t, optypep);
    __pack_decode_uintAny(pp, end, uint32_t, opsizep);
    return (0);
}

/*
 * __wt_logop_unpack --
 *     Read the operation type.
 */
int
__wt_logop_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *optypep, uint32_t *opsizep)
{
    WT_UNUSED(session);
    __pack_decode_uintAny(pp, end, uint32_t, optypep);
    __pack_decode_uintAny(pp, end, uint32_t, opsizep);
    return (0);
}

/*
 * __wt_logop_write --
 *     Write the operation type.
 */
int
__wt_logop_write(
  WT_SESSION_IMPL *session, uint8_t **pp, uint8_t *end, uint32_t optype, uint32_t opsize)
{
    WT_UNUSED(session);
    WT_RET(__pack_encode_uintAny(pp, end, optype));
    WT_RET(__pack_encode_uintAny(pp, end, opsize));
    return (0);
}

/*
 * __logrec_make_json_str --
 *     Unpack a string into JSON escaped format.
 */
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

/*
 * __logrec_make_hex_str --
 *     Convert data to a hexadecimal representation.
 */
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

/*
 * __wt_struct_size_col_modify --
 *     Calculate size of col_modify struct.
 */
static WT_INLINE size_t
__wt_struct_size_col_modify(uint32_t fileid, uint64_t recno, WT_ITEM *value)
{
    return (__wt_vsize_uint(fileid) + __wt_vsize_uint(recno) + value->size);
}

/*
 * __wt_struct_pack_col_modify --
 *     Pack the col_modify struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_col_modify(
  uint8_t **pp, uint8_t *end, uint32_t fileid, uint64_t recno, WT_ITEM *value)
{
    WT_RET(__pack_encode_uintAny(pp, end, fileid));
    WT_RET(__pack_encode_uintAny(pp, end, recno));
    WT_RET(__pack_encode_WT_ITEM_last(pp, end, value));

    return (0);
}

/*
 * __wt_struct_unpack_col_modify --
 *     Unpack the col_modify struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_col_modify(
  const uint8_t **pp, const uint8_t *end, uint32_t *fileidp, uint64_t *recnop, WT_ITEM *valuep)
{
    __pack_decode_uintAny(pp, end, uint32_t, fileidp);
    __pack_decode_uintAny(pp, end, uint64_t, recnop);
    __pack_decode_WT_ITEM_last(pp, end, valuep);

    return (0);
}

/*
 * __wt_logop_col_modify_pack --
 *     Pack the log operation col_modify.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_col_modify_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, uint64_t recno, WT_ITEM *value)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_col_modify(fileid, recno, value);
    size += __wt_vsize_uint(WT_LOGOP_COL_MODIFY) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, WT_LOGOP_COL_MODIFY, (uint32_t)size));
    WT_RET(__wt_struct_pack_col_modify(&buf, end, fileid, recno, value));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_col_modify_unpack --
 *     Unpack the log operation col_modify.
 */
int
__wt_logop_col_modify_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, uint64_t *recnop, WT_ITEM *valuep)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
      (ret = __wt_struct_unpack_col_modify(pp, end, fileidp, recnop, valuep)) != 0)
        WT_RET_MSG(session, ret, "logop_col_modify: unpack failure");

    WT_CHECK_OPTYPE(session, optype, WT_LOGOP_COL_MODIFY);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL, "logop_col_modify: size mismatch: expected %u, got %" PRIuPTR,
          size, WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}

/*
 * __wt_logop_col_modify_print --
 *     Print the log operation col_modify.
 */
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
    WT_ERR(__wt_fprintf(session, args->fs, "        \"fileid\": %" PRIu32 ",\n", fileid));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"fileid-hex\": \"0x%" PRIx32 "\",\n", fileid));
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

/*
 * __wt_struct_size_col_put --
 *     Calculate size of col_put struct.
 */
static WT_INLINE size_t
__wt_struct_size_col_put(uint32_t fileid, uint64_t recno, WT_ITEM *value)
{
    return (__wt_vsize_uint(fileid) + __wt_vsize_uint(recno) + value->size);
}

/*
 * __wt_struct_pack_col_put --
 *     Pack the col_put struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_col_put(
  uint8_t **pp, uint8_t *end, uint32_t fileid, uint64_t recno, WT_ITEM *value)
{
    WT_RET(__pack_encode_uintAny(pp, end, fileid));
    WT_RET(__pack_encode_uintAny(pp, end, recno));
    WT_RET(__pack_encode_WT_ITEM_last(pp, end, value));

    return (0);
}

/*
 * __wt_struct_unpack_col_put --
 *     Unpack the col_put struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_col_put(
  const uint8_t **pp, const uint8_t *end, uint32_t *fileidp, uint64_t *recnop, WT_ITEM *valuep)
{
    __pack_decode_uintAny(pp, end, uint32_t, fileidp);
    __pack_decode_uintAny(pp, end, uint64_t, recnop);
    __pack_decode_WT_ITEM_last(pp, end, valuep);

    return (0);
}

/*
 * __wt_logop_col_put_pack --
 *     Pack the log operation col_put.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_col_put_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, uint64_t recno, WT_ITEM *value)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_col_put(fileid, recno, value);
    size += __wt_vsize_uint(WT_LOGOP_COL_PUT) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, WT_LOGOP_COL_PUT, (uint32_t)size));
    WT_RET(__wt_struct_pack_col_put(&buf, end, fileid, recno, value));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_col_put_unpack --
 *     Unpack the log operation col_put.
 */
int
__wt_logop_col_put_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, uint64_t *recnop, WT_ITEM *valuep)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
      (ret = __wt_struct_unpack_col_put(pp, end, fileidp, recnop, valuep)) != 0)
        WT_RET_MSG(session, ret, "logop_col_put: unpack failure");

    WT_CHECK_OPTYPE(session, optype, WT_LOGOP_COL_PUT);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL, "logop_col_put: size mismatch: expected %u, got %" PRIuPTR,
          size, WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}

/*
 * __wt_logop_col_put_print --
 *     Print the log operation col_put.
 */
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
    WT_ERR(__wt_fprintf(session, args->fs, "        \"fileid\": %" PRIu32 ",\n", fileid));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"fileid-hex\": \"0x%" PRIx32 "\",\n", fileid));
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

/*
 * __wt_struct_size_col_remove --
 *     Calculate size of col_remove struct.
 */
static WT_INLINE size_t
__wt_struct_size_col_remove(uint32_t fileid, uint64_t recno)
{
    return (__wt_vsize_uint(fileid) + __wt_vsize_uint(recno));
}

/*
 * __wt_struct_pack_col_remove --
 *     Pack the col_remove struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_col_remove(uint8_t **pp, uint8_t *end, uint32_t fileid, uint64_t recno)
{
    WT_RET(__pack_encode_uintAny(pp, end, fileid));
    WT_RET(__pack_encode_uintAny(pp, end, recno));

    return (0);
}

/*
 * __wt_struct_unpack_col_remove --
 *     Unpack the col_remove struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_col_remove(
  const uint8_t **pp, const uint8_t *end, uint32_t *fileidp, uint64_t *recnop)
{
    __pack_decode_uintAny(pp, end, uint32_t, fileidp);
    __pack_decode_uintAny(pp, end, uint64_t, recnop);

    return (0);
}

/*
 * __wt_logop_col_remove_pack --
 *     Pack the log operation col_remove.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_col_remove_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, uint64_t recno)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_col_remove(fileid, recno);
    size += __wt_vsize_uint(WT_LOGOP_COL_REMOVE) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, WT_LOGOP_COL_REMOVE, (uint32_t)size));
    WT_RET(__wt_struct_pack_col_remove(&buf, end, fileid, recno));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_col_remove_unpack --
 *     Unpack the log operation col_remove.
 */
int
__wt_logop_col_remove_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, uint64_t *recnop)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
      (ret = __wt_struct_unpack_col_remove(pp, end, fileidp, recnop)) != 0)
        WT_RET_MSG(session, ret, "logop_col_remove: unpack failure");

    WT_CHECK_OPTYPE(session, optype, WT_LOGOP_COL_REMOVE);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL, "logop_col_remove: size mismatch: expected %u, got %" PRIuPTR,
          size, WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}

/*
 * __wt_logop_col_remove_print --
 *     Print the log operation col_remove.
 */
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
    WT_RET(__wt_fprintf(session, args->fs, "        \"fileid\": %" PRIu32 ",\n", fileid));
    WT_RET(__wt_fprintf(session, args->fs, "        \"fileid-hex\": \"0x%" PRIx32 "\",\n", fileid));
    WT_RET(__wt_fprintf(session, args->fs, "        \"recno\": %" PRIu64 "", recno));
    return (0);
}

/*
 * __wt_struct_size_col_truncate --
 *     Calculate size of col_truncate struct.
 */
static WT_INLINE size_t
__wt_struct_size_col_truncate(uint32_t fileid, uint64_t start, uint64_t stop)
{
    return (__wt_vsize_uint(fileid) + __wt_vsize_uint(start) + __wt_vsize_uint(stop));
}

/*
 * __wt_struct_pack_col_truncate --
 *     Pack the col_truncate struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_col_truncate(
  uint8_t **pp, uint8_t *end, uint32_t fileid, uint64_t start, uint64_t stop)
{
    WT_RET(__pack_encode_uintAny(pp, end, fileid));
    WT_RET(__pack_encode_uintAny(pp, end, start));
    WT_RET(__pack_encode_uintAny(pp, end, stop));

    return (0);
}

/*
 * __wt_struct_unpack_col_truncate --
 *     Unpack the col_truncate struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_col_truncate(
  const uint8_t **pp, const uint8_t *end, uint32_t *fileidp, uint64_t *startp, uint64_t *stopp)
{
    __pack_decode_uintAny(pp, end, uint32_t, fileidp);
    __pack_decode_uintAny(pp, end, uint64_t, startp);
    __pack_decode_uintAny(pp, end, uint64_t, stopp);

    return (0);
}

/*
 * __wt_logop_col_truncate_pack --
 *     Pack the log operation col_truncate.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_col_truncate_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, uint64_t start, uint64_t stop)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_col_truncate(fileid, start, stop);
    size += __wt_vsize_uint(WT_LOGOP_COL_TRUNCATE) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, WT_LOGOP_COL_TRUNCATE, (uint32_t)size));
    WT_RET(__wt_struct_pack_col_truncate(&buf, end, fileid, start, stop));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_col_truncate_unpack --
 *     Unpack the log operation col_truncate.
 */
int
__wt_logop_col_truncate_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, uint64_t *startp, uint64_t *stopp)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
      (ret = __wt_struct_unpack_col_truncate(pp, end, fileidp, startp, stopp)) != 0)
        WT_RET_MSG(session, ret, "logop_col_truncate: unpack failure");

    WT_CHECK_OPTYPE(session, optype, WT_LOGOP_COL_TRUNCATE);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL, "logop_col_truncate: size mismatch: expected %u, got %" PRIuPTR,
          size, WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}

/*
 * __wt_logop_col_truncate_print --
 *     Print the log operation col_truncate.
 */
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
    WT_RET(__wt_fprintf(session, args->fs, "        \"fileid\": %" PRIu32 ",\n", fileid));
    WT_RET(__wt_fprintf(session, args->fs, "        \"fileid-hex\": \"0x%" PRIx32 "\",\n", fileid));
    WT_RET(__wt_fprintf(session, args->fs, "        \"start\": %" PRIu64 ",\n", start));
    WT_RET(__wt_fprintf(session, args->fs, "        \"stop\": %" PRIu64 "", stop));
    return (0);
}

/*
 * __wt_struct_size_row_modify --
 *     Calculate size of row_modify struct.
 */
static WT_INLINE size_t
__wt_struct_size_row_modify(uint32_t fileid, WT_ITEM *key, WT_ITEM *value)
{
    return (__wt_vsize_uint(fileid) + __wt_vsize_uint(key->size) + key->size + value->size);
}

/*
 * __wt_struct_pack_row_modify --
 *     Pack the row_modify struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_row_modify(
  uint8_t **pp, uint8_t *end, uint32_t fileid, WT_ITEM *key, WT_ITEM *value)
{
    WT_RET(__pack_encode_uintAny(pp, end, fileid));
    WT_RET(__pack_encode_WT_ITEM(pp, end, key));
    WT_RET(__pack_encode_WT_ITEM_last(pp, end, value));

    return (0);
}

/*
 * __wt_struct_unpack_row_modify --
 *     Unpack the row_modify struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_row_modify(
  const uint8_t **pp, const uint8_t *end, uint32_t *fileidp, WT_ITEM *keyp, WT_ITEM *valuep)
{
    __pack_decode_uintAny(pp, end, uint32_t, fileidp);
    __pack_decode_WT_ITEM(pp, end, keyp);
    __pack_decode_WT_ITEM_last(pp, end, valuep);

    return (0);
}

/*
 * __wt_logop_row_modify_pack --
 *     Pack the log operation row_modify.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_row_modify_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, WT_ITEM *key, WT_ITEM *value)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_row_modify(fileid, key, value);
    size += __wt_vsize_uint(WT_LOGOP_ROW_MODIFY) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, WT_LOGOP_ROW_MODIFY, (uint32_t)size));
    WT_RET(__wt_struct_pack_row_modify(&buf, end, fileid, key, value));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_row_modify_unpack --
 *     Unpack the log operation row_modify.
 */
int
__wt_logop_row_modify_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, WT_ITEM *keyp, WT_ITEM *valuep)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
      (ret = __wt_struct_unpack_row_modify(pp, end, fileidp, keyp, valuep)) != 0)
        WT_RET_MSG(session, ret, "logop_row_modify: unpack failure");

    WT_CHECK_OPTYPE(session, optype, WT_LOGOP_ROW_MODIFY);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL, "logop_row_modify: size mismatch: expected %u, got %" PRIuPTR,
          size, WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}

/*
 * __wt_logop_row_modify_print --
 *     Print the log operation row_modify.
 */
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
    WT_ERR(__wt_fprintf(session, args->fs, "        \"fileid\": %" PRIu32 ",\n", fileid));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"fileid-hex\": \"0x%" PRIx32 "\",\n", fileid));
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

/*
 * __wt_struct_size_row_put --
 *     Calculate size of row_put struct.
 */
static WT_INLINE size_t
__wt_struct_size_row_put(uint32_t fileid, WT_ITEM *key, WT_ITEM *value)
{
    return (__wt_vsize_uint(fileid) + __wt_vsize_uint(key->size) + key->size + value->size);
}

/*
 * __wt_struct_pack_row_put --
 *     Pack the row_put struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_row_put(uint8_t **pp, uint8_t *end, uint32_t fileid, WT_ITEM *key, WT_ITEM *value)
{
    WT_RET(__pack_encode_uintAny(pp, end, fileid));
    WT_RET(__pack_encode_WT_ITEM(pp, end, key));
    WT_RET(__pack_encode_WT_ITEM_last(pp, end, value));

    return (0);
}

/*
 * __wt_struct_unpack_row_put --
 *     Unpack the row_put struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_row_put(
  const uint8_t **pp, const uint8_t *end, uint32_t *fileidp, WT_ITEM *keyp, WT_ITEM *valuep)
{
    __pack_decode_uintAny(pp, end, uint32_t, fileidp);
    __pack_decode_WT_ITEM(pp, end, keyp);
    __pack_decode_WT_ITEM_last(pp, end, valuep);

    return (0);
}

/*
 * __wt_logop_row_put_pack --
 *     Pack the log operation row_put.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_row_put_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, WT_ITEM *key, WT_ITEM *value)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_row_put(fileid, key, value);
    size += __wt_vsize_uint(WT_LOGOP_ROW_PUT) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, WT_LOGOP_ROW_PUT, (uint32_t)size));
    WT_RET(__wt_struct_pack_row_put(&buf, end, fileid, key, value));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_row_put_unpack --
 *     Unpack the log operation row_put.
 */
int
__wt_logop_row_put_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, WT_ITEM *keyp, WT_ITEM *valuep)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
      (ret = __wt_struct_unpack_row_put(pp, end, fileidp, keyp, valuep)) != 0)
        WT_RET_MSG(session, ret, "logop_row_put: unpack failure");

    WT_CHECK_OPTYPE(session, optype, WT_LOGOP_ROW_PUT);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL, "logop_row_put: size mismatch: expected %u, got %" PRIuPTR,
          size, WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}

/*
 * __wt_logop_row_put_print --
 *     Print the log operation row_put.
 */
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
    WT_ERR(__wt_fprintf(session, args->fs, "        \"fileid\": %" PRIu32 ",\n", fileid));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"fileid-hex\": \"0x%" PRIx32 "\",\n", fileid));
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

/*
 * __wt_struct_size_row_remove --
 *     Calculate size of row_remove struct.
 */
static WT_INLINE size_t
__wt_struct_size_row_remove(uint32_t fileid, WT_ITEM *key)
{
    return (__wt_vsize_uint(fileid) + key->size);
}

/*
 * __wt_struct_pack_row_remove --
 *     Pack the row_remove struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_row_remove(uint8_t **pp, uint8_t *end, uint32_t fileid, WT_ITEM *key)
{
    WT_RET(__pack_encode_uintAny(pp, end, fileid));
    WT_RET(__pack_encode_WT_ITEM_last(pp, end, key));

    return (0);
}

/*
 * __wt_struct_unpack_row_remove --
 *     Unpack the row_remove struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_row_remove(
  const uint8_t **pp, const uint8_t *end, uint32_t *fileidp, WT_ITEM *keyp)
{
    __pack_decode_uintAny(pp, end, uint32_t, fileidp);
    __pack_decode_WT_ITEM_last(pp, end, keyp);

    return (0);
}

/*
 * __wt_logop_row_remove_pack --
 *     Pack the log operation row_remove.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_row_remove_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid, WT_ITEM *key)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_row_remove(fileid, key);
    size += __wt_vsize_uint(WT_LOGOP_ROW_REMOVE) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, WT_LOGOP_ROW_REMOVE, (uint32_t)size));
    WT_RET(__wt_struct_pack_row_remove(&buf, end, fileid, key));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_row_remove_unpack --
 *     Unpack the log operation row_remove.
 */
int
__wt_logop_row_remove_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, WT_ITEM *keyp)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
      (ret = __wt_struct_unpack_row_remove(pp, end, fileidp, keyp)) != 0)
        WT_RET_MSG(session, ret, "logop_row_remove: unpack failure");

    WT_CHECK_OPTYPE(session, optype, WT_LOGOP_ROW_REMOVE);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL, "logop_row_remove: size mismatch: expected %u, got %" PRIuPTR,
          size, WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}

/*
 * __wt_logop_row_remove_print --
 *     Print the log operation row_remove.
 */
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
    WT_ERR(__wt_fprintf(session, args->fs, "        \"fileid\": %" PRIu32 ",\n", fileid));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"fileid-hex\": \"0x%" PRIx32 "\",\n", fileid));
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

/*
 * __wt_struct_size_row_truncate --
 *     Calculate size of row_truncate struct.
 */
static WT_INLINE size_t
__wt_struct_size_row_truncate(uint32_t fileid, WT_ITEM *start, WT_ITEM *stop, uint32_t mode)
{
    return (__wt_vsize_uint(fileid) + __wt_vsize_uint(start->size) + start->size +
      __wt_vsize_uint(stop->size) + stop->size + __wt_vsize_uint(mode));
}

/*
 * __wt_struct_pack_row_truncate --
 *     Pack the row_truncate struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_row_truncate(
  uint8_t **pp, uint8_t *end, uint32_t fileid, WT_ITEM *start, WT_ITEM *stop, uint32_t mode)
{
    WT_RET(__pack_encode_uintAny(pp, end, fileid));
    WT_RET(__pack_encode_WT_ITEM(pp, end, start));
    WT_RET(__pack_encode_WT_ITEM(pp, end, stop));
    WT_RET(__pack_encode_uintAny(pp, end, mode));

    return (0);
}

/*
 * __wt_struct_unpack_row_truncate --
 *     Unpack the row_truncate struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_row_truncate(const uint8_t **pp, const uint8_t *end, uint32_t *fileidp,
  WT_ITEM *startp, WT_ITEM *stopp, uint32_t *modep)
{
    __pack_decode_uintAny(pp, end, uint32_t, fileidp);
    __pack_decode_WT_ITEM(pp, end, startp);
    __pack_decode_WT_ITEM(pp, end, stopp);
    __pack_decode_uintAny(pp, end, uint32_t, modep);

    return (0);
}

/*
 * __wt_logop_row_truncate_pack --
 *     Pack the log operation row_truncate.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_row_truncate_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  WT_ITEM *start, WT_ITEM *stop, uint32_t mode)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_row_truncate(fileid, start, stop, mode);
    size += __wt_vsize_uint(WT_LOGOP_ROW_TRUNCATE) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, WT_LOGOP_ROW_TRUNCATE, (uint32_t)size));
    WT_RET(__wt_struct_pack_row_truncate(&buf, end, fileid, start, stop, mode));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_row_truncate_unpack --
 *     Unpack the log operation row_truncate.
 */
int
__wt_logop_row_truncate_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *fileidp, WT_ITEM *startp, WT_ITEM *stopp, uint32_t *modep)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
      (ret = __wt_struct_unpack_row_truncate(pp, end, fileidp, startp, stopp, modep)) != 0)
        WT_RET_MSG(session, ret, "logop_row_truncate: unpack failure");

    WT_CHECK_OPTYPE(session, optype, WT_LOGOP_ROW_TRUNCATE);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL, "logop_row_truncate: size mismatch: expected %u, got %" PRIuPTR,
          size, WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}

/*
 * __wt_logop_row_truncate_print --
 *     Print the log operation row_truncate.
 */
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
    WT_ERR(__wt_fprintf(session, args->fs, "        \"fileid\": %" PRIu32 ",\n", fileid));
    WT_ERR(__wt_fprintf(session, args->fs, "        \"fileid-hex\": \"0x%" PRIx32 "\",\n", fileid));
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

/*
 * __wt_struct_size_checkpoint_start --
 *     Calculate size of checkpoint_start struct.
 */
static WT_INLINE size_t
__wt_struct_size_checkpoint_start(void)
{
    return (0);
}

/*
 * __wt_struct_pack_checkpoint_start --
 *     Pack the checkpoint_start struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_checkpoint_start(uint8_t **pp, uint8_t *end)
{
    WT_UNUSED(pp);
    WT_UNUSED(end);
    return (0);
}

/*
 * __wt_struct_unpack_checkpoint_start --
 *     Unpack the checkpoint_start struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_checkpoint_start(const uint8_t **pp, const uint8_t *end)
{
    WT_UNUSED(pp);
    WT_UNUSED(end);
    return (0);
}

/*
 * __wt_logop_checkpoint_start_pack --
 *     Pack the log operation checkpoint_start.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_checkpoint_start_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_checkpoint_start();
    size += __wt_vsize_uint(WT_LOGOP_CHECKPOINT_START) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, WT_LOGOP_CHECKPOINT_START, (uint32_t)size));
    WT_RET(__wt_struct_pack_checkpoint_start(&buf, end));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_checkpoint_start_unpack --
 *     Unpack the log operation checkpoint_start.
 */
int
__wt_logop_checkpoint_start_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
      (ret = __wt_struct_unpack_checkpoint_start(pp, end)) != 0)
        WT_RET_MSG(session, ret, "logop_checkpoint_start: unpack failure");

    WT_CHECK_OPTYPE(session, optype, WT_LOGOP_CHECKPOINT_START);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL,
          "logop_checkpoint_start: size mismatch: expected %u, got %" PRIuPTR, size,
          WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}

/*
 * __wt_logop_checkpoint_start_print --
 *     Print the log operation checkpoint_start.
 */
int
__wt_logop_checkpoint_start_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{

    WT_RET(__wt_logop_checkpoint_start_unpack(session, pp, end));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"checkpoint_start\"\n"));

    return (0);
}

/*
 * __wt_struct_size_prev_lsn --
 *     Calculate size of prev_lsn struct.
 */
static WT_INLINE size_t
__wt_struct_size_prev_lsn(WT_LSN *prev_lsn)
{
    return (__wt_vsize_uint(prev_lsn->l.file) + __wt_vsize_uint(prev_lsn->l.offset));
}

/*
 * __wt_struct_pack_prev_lsn --
 *     Pack the prev_lsn struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_prev_lsn(uint8_t **pp, uint8_t *end, WT_LSN *prev_lsn)
{
    WT_RET(__pack_encode_uintAny(pp, end, prev_lsn->l.file));
    WT_RET(__pack_encode_uintAny(pp, end, prev_lsn->l.offset));

    return (0);
}

/*
 * __wt_struct_unpack_prev_lsn --
 *     Unpack the prev_lsn struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_prev_lsn(const uint8_t **pp, const uint8_t *end, WT_LSN *prev_lsnp)
{
    __pack_decode_uintAny(pp, end, uint32_t, &prev_lsnp->l.file);
    __pack_decode_uintAny(pp, end, uint32_t, &prev_lsnp->l.offset);

    return (0);
}

/*
 * __wt_logop_prev_lsn_pack --
 *     Pack the log operation prev_lsn.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_prev_lsn_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *prev_lsn)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_prev_lsn(prev_lsn);
    size += __wt_vsize_uint(WT_LOGOP_PREV_LSN) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, WT_LOGOP_PREV_LSN, (uint32_t)size));
    WT_RET(__wt_struct_pack_prev_lsn(&buf, end, prev_lsn));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_prev_lsn_unpack --
 *     Unpack the log operation prev_lsn.
 */
int
__wt_logop_prev_lsn_unpack(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_LSN *prev_lsnp)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
      (ret = __wt_struct_unpack_prev_lsn(pp, end, prev_lsnp)) != 0)
        WT_RET_MSG(session, ret, "logop_prev_lsn: unpack failure");

    WT_CHECK_OPTYPE(session, optype, WT_LOGOP_PREV_LSN);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL, "logop_prev_lsn: size mismatch: expected %u, got %" PRIuPTR,
          size, WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}

/*
 * __wt_logop_prev_lsn_print --
 *     Print the log operation prev_lsn.
 */
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

/*
 * __wt_struct_size_backup_id --
 *     Calculate size of backup_id struct.
 */
static WT_INLINE size_t
__wt_struct_size_backup_id(uint32_t index, uint64_t granularity, const char *id)
{
    return (__wt_vsize_uint(index) + __wt_vsize_uint(granularity) + strlen(id) + 1);
}

/*
 * __wt_struct_pack_backup_id --
 *     Pack the backup_id struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_backup_id(
  uint8_t **pp, uint8_t *end, uint32_t index, uint64_t granularity, const char *id)
{
    WT_RET(__pack_encode_uintAny(pp, end, index));
    WT_RET(__pack_encode_uintAny(pp, end, granularity));
    WT_RET(__pack_encode_string(pp, end, id));

    return (0);
}

/*
 * __wt_struct_unpack_backup_id --
 *     Unpack the backup_id struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_backup_id(const uint8_t **pp, const uint8_t *end, uint32_t *indexp,
  uint64_t *granularityp, const char **idp)
{
    __pack_decode_uintAny(pp, end, uint32_t, indexp);
    __pack_decode_uintAny(pp, end, uint64_t, granularityp);
    __pack_decode_string(pp, end, idp);

    return (0);
}

/*
 * __wt_logop_backup_id_pack --
 *     Pack the log operation backup_id.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_backup_id_pack(
  WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t index, uint64_t granularity, const char *id)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_backup_id(index, granularity, id);
    size += __wt_vsize_uint(WT_LOGOP_BACKUP_ID) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, WT_LOGOP_BACKUP_ID, (uint32_t)size));
    WT_RET(__wt_struct_pack_backup_id(&buf, end, index, granularity, id));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_backup_id_unpack --
 *     Unpack the log operation backup_id.
 */
int
__wt_logop_backup_id_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *indexp, uint64_t *granularityp, const char **idp)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
      (ret = __wt_struct_unpack_backup_id(pp, end, indexp, granularityp, idp)) != 0)
        WT_RET_MSG(session, ret, "logop_backup_id: unpack failure");

    WT_CHECK_OPTYPE(session, optype, WT_LOGOP_BACKUP_ID);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL, "logop_backup_id: size mismatch: expected %u, got %" PRIuPTR,
          size, WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}

/*
 * __wt_logop_backup_id_print --
 *     Print the log operation backup_id.
 */
int
__wt_logop_backup_id_print(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    uint32_t index;
    uint64_t granularity;
    const char *id;

    WT_RET(__wt_logop_backup_id_unpack(session, pp, end, &index, &granularity, &id));

    WT_RET(__wt_fprintf(session, args->fs, " \"optype\": \"backup_id\",\n"));
    WT_RET(__wt_fprintf(session, args->fs, "        \"index\": %" PRIu32 ",\n", index));
    WT_RET(__wt_fprintf(session, args->fs, "        \"granularity\": %" PRIu64 ",\n", granularity));
    WT_RET(__wt_fprintf(session, args->fs, "        \"id\": \"%s\"", id));
    return (0);
}

/*
 * __wt_struct_size_txn_timestamp --
 *     Calculate size of txn_timestamp struct.
 */
static WT_INLINE size_t
__wt_struct_size_txn_timestamp(uint64_t time_sec, uint64_t time_nsec, uint64_t commit_ts,
  uint64_t durable_ts, uint64_t first_commit_ts, uint64_t prepare_ts, uint64_t read_ts)
{
    return (__wt_vsize_uint(time_sec) + __wt_vsize_uint(time_nsec) + __wt_vsize_uint(commit_ts) +
      __wt_vsize_uint(durable_ts) + __wt_vsize_uint(first_commit_ts) + __wt_vsize_uint(prepare_ts) +
      __wt_vsize_uint(read_ts));
}

/*
 * __wt_struct_pack_txn_timestamp --
 *     Pack the txn_timestamp struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_txn_timestamp(uint8_t **pp, uint8_t *end, uint64_t time_sec, uint64_t time_nsec,
  uint64_t commit_ts, uint64_t durable_ts, uint64_t first_commit_ts, uint64_t prepare_ts,
  uint64_t read_ts)
{
    WT_RET(__pack_encode_uintAny(pp, end, time_sec));
    WT_RET(__pack_encode_uintAny(pp, end, time_nsec));
    WT_RET(__pack_encode_uintAny(pp, end, commit_ts));
    WT_RET(__pack_encode_uintAny(pp, end, durable_ts));
    WT_RET(__pack_encode_uintAny(pp, end, first_commit_ts));
    WT_RET(__pack_encode_uintAny(pp, end, prepare_ts));
    WT_RET(__pack_encode_uintAny(pp, end, read_ts));

    return (0);
}

/*
 * __wt_struct_unpack_txn_timestamp --
 *     Unpack the txn_timestamp struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_txn_timestamp(const uint8_t **pp, const uint8_t *end, uint64_t *time_secp,
  uint64_t *time_nsecp, uint64_t *commit_tsp, uint64_t *durable_tsp, uint64_t *first_commit_tsp,
  uint64_t *prepare_tsp, uint64_t *read_tsp)
{
    __pack_decode_uintAny(pp, end, uint64_t, time_secp);
    __pack_decode_uintAny(pp, end, uint64_t, time_nsecp);
    __pack_decode_uintAny(pp, end, uint64_t, commit_tsp);
    __pack_decode_uintAny(pp, end, uint64_t, durable_tsp);
    __pack_decode_uintAny(pp, end, uint64_t, first_commit_tsp);
    __pack_decode_uintAny(pp, end, uint64_t, prepare_tsp);
    __pack_decode_uintAny(pp, end, uint64_t, read_tsp);

    return (0);
}

/*
 * __wt_logop_txn_timestamp_pack --
 *     Pack the log operation txn_timestamp.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_txn_timestamp_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint64_t time_sec,
  uint64_t time_nsec, uint64_t commit_ts, uint64_t durable_ts, uint64_t first_commit_ts,
  uint64_t prepare_ts, uint64_t read_ts)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_txn_timestamp(
      time_sec, time_nsec, commit_ts, durable_ts, first_commit_ts, prepare_ts, read_ts);
    size += __wt_vsize_uint(WT_LOGOP_TXN_TIMESTAMP) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, WT_LOGOP_TXN_TIMESTAMP, (uint32_t)size));
    WT_RET(__wt_struct_pack_txn_timestamp(
      &buf, end, time_sec, time_nsec, commit_ts, durable_ts, first_commit_ts, prepare_ts, read_ts));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_txn_timestamp_unpack --
 *     Unpack the log operation txn_timestamp.
 */
int
__wt_logop_txn_timestamp_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint64_t *time_secp, uint64_t *time_nsecp, uint64_t *commit_tsp, uint64_t *durable_tsp,
  uint64_t *first_commit_tsp, uint64_t *prepare_tsp, uint64_t *read_tsp)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
      (ret = __wt_struct_unpack_txn_timestamp(pp, end, time_secp, time_nsecp, commit_tsp,
         durable_tsp, first_commit_tsp, prepare_tsp, read_tsp)) != 0)
        WT_RET_MSG(session, ret, "logop_txn_timestamp: unpack failure");

    WT_CHECK_OPTYPE(session, optype, WT_LOGOP_TXN_TIMESTAMP);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL,
          "logop_txn_timestamp: size mismatch: expected %u, got %" PRIuPTR, size,
          WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}

/*
 * __wt_logop_txn_timestamp_print --
 *     Print the log operation txn_timestamp.
 */
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

/*
 * __wt_txn_op_printlog --
 *     Print operation from a log cookie.
 */
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

    case WT_LOGOP_BACKUP_ID:
        WT_RET(__wt_logop_backup_id_print(session, pp, end, args));
        break;

    case WT_LOGOP_TXN_TIMESTAMP:
        WT_RET(__wt_logop_txn_timestamp_print(session, pp, end, args));
        break;

    default:
        return (__wt_illegal_value(session, optype));
    }

    return (0);
}
