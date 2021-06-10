/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define WT_OPTRACK_MAXRECS (16384)
#define WT_OPTRACK_BUFSIZE (WT_OPTRACK_MAXRECS * sizeof(WT_OPTRACK_RECORD))
#define WT_OPTRACK_VERSION 3

/*
 * WT_OPTRACK_HEADER --
 *     A header in the operation tracking log file. The internal session
 *     identifier is a boolean: 1 if the session is internal, 0 otherwise.
 */
struct __wt_optrack_header {
    uint32_t optrack_version;
    uint32_t optrack_session_internal;
    uint32_t optrack_tsc_nsec_ratio;
    uint32_t padding;
    uint64_t optrack_seconds_epoch;
};

/*
 * WT_OPTRACK_RECORD --
 *     A structure for logging function entry and exit events.
 *
 * We pad the record so that the size of the entire record is 16 bytes. If we
 * don't do this, the compiler will pad it for us, because we keep records in
 * the record buffer array and each new record must be aligned on the 8-byte
 * boundary, since its first element is an 8-byte timestamp. Instead of letting
 * the compiler insert the padding silently, we pad explicitly, so that whoever
 * writes the binary decoder can refer to this struct to find out the record
 * size.
 *
 * The operation id included in this structure is a unique address of a function
 * in the binary. As we log operations, we keep track of the correspondence
 * between function addresses and their names. When the log file is decoded,
 * operations identifiers are replaced with function names. Therefore, the
 * present design assumes that the user will be inserting the tracking macros
 * on function boundaries: when we enter into the function and when we exit
 * from it.
 */
struct __wt_optrack_record {
    uint64_t op_timestamp; /* timestamp */
    uint16_t op_id;        /* function ID */
    uint16_t op_type;      /* start/stop */
    uint8_t padding[4];
};

#define WT_TRACK_OP(s, optype)                                                \
    do {                                                                      \
        WT_OPTRACK_RECORD *__tr;                                              \
        __tr = &((s)->optrack_buf[(s)->optrackbuf_ptr % WT_OPTRACK_MAXRECS]); \
        __tr->op_timestamp = __wt_clock(s);                                   \
        __tr->op_id = __func_id;                                              \
        __tr->op_type = optype;                                               \
                                                                              \
        if (++(s)->optrackbuf_ptr == WT_OPTRACK_MAXRECS) {                    \
            __wt_optrack_flush_buffer(s);                                     \
            (s)->optrackbuf_ptr = 0;                                          \
        }                                                                     \
    } while (0)

/*
 * We do not synchronize access to optrack buffer pointer under the assumption that there is no more
 * than one thread using a given session. This assumption does not always hold. When it does not, we
 * might have a race. In this case, we may lose a few log records. We prefer to risk losing a few
 * log records occasionally in order not to synchronize this code, which is intended to be very
 * lightweight. Exclude the default session (ID 0) because it can be used by multiple threads and it
 * is also used in error paths during failed open calls.
 */
#define WT_TRACK_OP_DECL static uint16_t __func_id = 0
#define WT_TRACK_OP_INIT(s)                                                 \
    if (F_ISSET(S2C(s), WT_CONN_OPTRACK) && (s)->id != 0) {                 \
        if (__func_id == 0)                                                 \
            __wt_optrack_record_funcid(s, __PRETTY_FUNCTION__, &__func_id); \
        WT_TRACK_OP(s, 0);                                                  \
    }

#define WT_TRACK_OP_END(s)                                \
    if (F_ISSET(S2C(s), WT_CONN_OPTRACK) && (s)->id != 0) \
        WT_TRACK_OP(s, 1);
