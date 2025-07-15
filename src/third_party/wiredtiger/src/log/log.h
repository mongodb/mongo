/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 * All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_LOGSCAN_FIRST 0x01u
#define WT_LOGSCAN_FROM_CKP 0x02u
#define WT_LOGSCAN_ONE 0x04u
#define WT_LOGSCAN_RECOVER 0x08u
#define WT_LOGSCAN_RECOVER_METADATA 0x10u
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_LOG_DSYNC 0x1u
#define WT_LOG_FLUSH 0x2u
#define WT_LOG_FSYNC 0x4u
#define WT_LOG_SYNC_ENABLED 0x8u
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

#define WT_LOGOP_IGNORE 0x80000000
#define WT_LOGOP_IS_IGNORED(val) ((val)&WT_LOGOP_IGNORE)

/*
 * WT_LSN --
 *	A log sequence number, representing a position in the transaction log.
 */
union __wt_lsn {
    struct {
#ifdef WORDS_BIGENDIAN
        uint32_t file;
        uint32_t offset;
#else
        wt_shared uint32_t offset;
        uint32_t file;
#endif
    } l;
    wt_shared uint64_t file_offset;
};

#define WT_LOG_FILENAME "WiredTigerLog" /* Log file name */

#define WT_MAX_LSN_STRING 32

/*
 * Atomically set the LSN. There are two forms. We need WT_ASSIGN_LSN because some compilers (at
 * least clang address sanitizer) does not do atomic 64-bit structure assignment so we need to
 * explicitly assign the 64-bit field. And WT_SET_LSN atomically sets the LSN given a file/offset.
 */
#define WT_ASSIGN_LSN(dstl, srcl) \
    __wt_atomic_store64(&(dstl)->file_offset, __wt_atomic_load64(&(srcl)->file_offset))
#define WT_SET_LSN(l, f, o) __wt_atomic_store64(&(l)->file_offset, (((uint64_t)(f) << 32) + (o)))

#define WT_INIT_LSN_FILE 1
#define WT_INIT_LSN(l) WT_SET_LSN((l), WT_INIT_LSN_FILE, 0)

#define WT_MAX_LSN(l) WT_SET_LSN((l), UINT32_MAX, INT32_MAX)

#define WT_ZERO_LSN(l) WT_SET_LSN((l), 0, 0)

/*
 * Test for initial LSN. We only need to shift the 1 for comparison.
 */
#define WT_IS_INIT_LSN(l) (__wt_atomic_load64(&(l)->file_offset) == ((uint64_t)1 << 32))
/*
 * Original tested INT32_MAX. But if we read one from an older release we may see UINT32_MAX.
 */
#define WT_IS_MAX_LSN(lsn) \
    ((lsn)->l.file == UINT32_MAX && ((lsn)->l.offset == INT32_MAX || (lsn)->l.offset == UINT32_MAX))
/*
 * Test for zero LSN.
 */
#define WT_IS_ZERO_LSN(l) (__wt_atomic_load64(&(l)->file_offset) == 0)

#define WT_LOG_SKIP_HEADER(data) ((const uint8_t *)(data) + offsetof(WT_LOG_RECORD, record))

/*
 * Size range for the log files.
 */
#define WT_LOG_FILE_MAX ((int64_t)2 * WT_GIGABYTE)
#define WT_LOG_FILE_MIN (100 * WT_KILOBYTE)

struct __wt_log_record {
    uint32_t len;      /* 00-03: Record length including hdr */
    uint32_t checksum; /* 04-07: Checksum of the record */

/*
 * No automatic generation: flag values cannot change, they're written to disk.
 *
 * Unused bits in the flags, as well as the 'unused' padding, are expected to be zeroed; we check
 * that to help detect file corruption.
 */
#define WT_LOG_RECORD_COMPRESSED 0x01u /* Compressed except hdr */
#define WT_LOG_RECORD_ENCRYPTED 0x02u  /* Encrypted except hdr */
#define WT_LOG_RECORD_ALL_FLAGS (WT_LOG_RECORD_COMPRESSED | WT_LOG_RECORD_ENCRYPTED)
    uint16_t flags;    /* 08-09: Flags */
    uint8_t unused[2]; /* 10-11: Padding */
    uint32_t mem_len;  /* 12-15: Uncompressed len if needed */
    uint8_t record[0]; /* Beginning of actual data */
};

/*
 * WiredTiger release version where log format version changed.
 *
 * FIXME WT-8681 - According to WT_MIN_STARTUP_VERSION any WT version less then 3.2.0 will not
 * start. Can we drop V2, V3 here?
 */
#define WT_LOG_V2_VERSION ((WT_VERSION){3, 0, 0})
#define WT_LOG_V3_VERSION ((WT_VERSION){3, 1, 0})
#define WT_LOG_V4_VERSION ((WT_VERSION){3, 3, 0})
#define WT_LOG_V5_VERSION ((WT_VERSION){10, 0, 0})

/* Cookie passed through the transaction printlog routines. */
struct __wt_txn_printlog_args {
    WT_FSTREAM *fs;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TXN_PRINTLOG_HEX 0x1u      /* Add hex output */
#define WT_TXN_PRINTLOG_MSG 0x2u      /* Messages only */
#define WT_TXN_PRINTLOG_UNREDACT 0x4u /* Don't redact user data from output */
                                      /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

struct __wt_log_thread {
    WT_CONDVAR *cond;         /* wait mutex */
    WT_SESSION_IMPL *session; /* session associated with thread */
    wt_thread_t tid;          /* thread id*/
    bool tid_set;             /* thread set */
};

struct __wt_log_manager {
    WT_RWLOCK debug_log_retention_lock; /* Log retention reconfiguration lock */

    WTI_LOG *log; /* Logging structure */

    WT_COMPRESSOR *compressor; /* configuration : Logging compressor */

    wt_off_t dirty_max;             /* configuration : Log dirty system cache max size */
    wt_off_t extend_len;            /* configuration : file_extend log length */
    wt_off_t file_max;              /* configuration : Log file max size */
    uint32_t force_write_wait;      /* configuration : Log force write wait */
    const char *log_path;           /* configuration : Logging path format */
    wt_shared uint32_t txn_logsync; /* configuration : Log sync */

    wt_shared uint32_t cursors;   /* Private : Log cursor count */
    uint32_t prealloc;            /* Private : Log file pre-allocation */
    uint32_t prealloc_init_count; /* Private : initial number of pre-allocated log files */
    uint16_t req_max;             /* Private : Max required log version */
    uint16_t req_min;             /* Private : Min required log version */

    WT_LOG_THREAD file;   /* Private : file thread */
    WT_LOG_THREAD server; /* Private : server thread */
    WT_LOG_THREAD wrlsn;  /* Private : write lsn thread */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_LOG_CONFIG_ENABLED 0x001u  /* Logging is configured */
#define WT_LOG_DOWNGRADED 0x002u      /* Running older version */
#define WT_LOG_ENABLED 0x004u         /* Logging is enabled */
#define WT_LOG_EXISTED 0x008u         /* Log files found */
#define WT_LOG_FORCE_DOWNGRADE 0x010u /* Force downgrade */
#define WT_LOG_INCR_BACKUP 0x020u     /* Incremental backup log required */
#define WT_LOG_RECOVER_DIRTY 0x040u   /* Recovering unclean */
#define WT_LOG_RECOVER_DONE 0x080u    /* Recovery completed */
#define WT_LOG_RECOVER_ERR 0x100u     /* Error if recovery required */
#define WT_LOG_RECOVER_FAILED 0x200u  /* Recovery failed */
#define WT_LOG_REMOVE 0x400u          /* Removal is enabled */
#define WT_LOG_ZERO_FILL 0x800u       /* Manually zero files */
                                      /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

    uint32_t flags; /* Global logging configuration */
};

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

extern int __wt_curlog_open(WT_SESSION_IMPL *session, const char *uri, const char *cfg[],
  WT_CURSOR **cursorp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_compat_verify(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_filename(WT_SESSION_IMPL *session, uint32_t id, const char *file_prefix,
  WT_ITEM *buf) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_flush(WT_SESSION_IMPL *session, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_flush_lsn(WT_SESSION_IMPL *session, WT_LSN *lsn, bool start)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_force_sync(WT_SESSION_IMPL *session, WT_LSN *min_lsn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_get_backup_files(WT_SESSION_IMPL *session, char ***filesp, u_int *countp,
  uint32_t *maxid, bool active_only) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_needs_recovery(WT_SESSION_IMPL *session, WT_LSN *ckp_lsn, bool *recp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_printf(WT_SESSION_IMPL *session, const char *format, ...)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_reset(WT_SESSION_IMPL *session, uint32_t lognum)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_scan(WT_SESSION_IMPL *session, WT_LSN *start_lsnp, WT_LSN *end_lsnp,
  uint32_t flags,
  int (*func)(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp, WT_LSN *next_lsnp,
    void *cookie, int firstrecord),
  void *cookie) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_system_backup_id(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_truncate_files(WT_SESSION_IMPL *session, WT_CURSOR *cursor, bool force)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_vprintf(WT_SESSION_IMPL *session, const char *fmt, va_list ap)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_log_write(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp, uint32_t flags)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logmgr_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logmgr_create(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logmgr_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logmgr_open(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logmgr_reconfig(WT_SESSION_IMPL *session, const char **cfg)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_backup_id_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t index,
  uint64_t granularity, const char *id) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_backup_id_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_backup_id_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *indexp, uint64_t *granularityp, const char **idp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_checkpoint_start_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_checkpoint_start_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_checkpoint_start_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_modify_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  uint64_t recno, WT_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_modify_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_modify_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, uint64_t *recnop, WT_ITEM *valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_put_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  uint64_t recno, WT_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_put_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_put_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, uint64_t *recnop, WT_ITEM *valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_remove_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  uint64_t recno) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_remove_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_remove_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, uint64_t *recnop)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_truncate_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  uint64_t start, uint64_t stop) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_truncate_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_col_truncate_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, uint64_t *startp, uint64_t *stopp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_prev_lsn_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *prev_lsn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_prev_lsn_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_prev_lsn_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_LSN *prev_lsnp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_read(WT_SESSION_IMPL *session, const uint8_t **pp_peek, const uint8_t *end,
  uint32_t *optypep, uint32_t *opsizep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_modify_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  WT_ITEM *key, WT_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_modify_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_modify_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, WT_ITEM *keyp, WT_ITEM *valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_put_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  WT_ITEM *key, WT_ITEM *value) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_put_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_put_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, WT_ITEM *keyp, WT_ITEM *valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_remove_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  WT_ITEM *key) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_remove_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_remove_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, WT_ITEM *keyp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_truncate_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec, uint32_t fileid,
  WT_ITEM *start, WT_ITEM *stop, uint32_t mode) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_truncate_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_row_truncate_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint32_t *fileidp, WT_ITEM *startp, WT_ITEM *stopp, uint32_t *modep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_txn_timestamp_pack(WT_SESSION_IMPL *session, WT_ITEM *logrec,
  uint64_t time_sec, uint64_t time_nsec, uint64_t commit_ts, uint64_t durable_ts,
  uint64_t first_commit_ts, uint64_t prepare_ts, uint64_t read_ts)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_txn_timestamp_print(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_txn_timestamp_unpack(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, uint64_t *time_secp, uint64_t *time_nsecp, uint64_t *commit_tsp,
  uint64_t *durable_tsp, uint64_t *first_commit_tsp, uint64_t *prepare_tsp, uint64_t *read_tsp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *optypep, uint32_t *opsizep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logop_write(WT_SESSION_IMPL *session, uint8_t **pp, uint8_t *end, uint32_t optype,
  uint32_t opsize) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logrec_alloc(WT_SESSION_IMPL *session, size_t size, WT_ITEM **logrecp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_logrec_read(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *rectypep) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_txn_op_printlog(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  WT_TXN_PRINTLOG_ARGS *args) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_verbose_dump_log(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void __wt_log_ckpt(WT_SESSION_IMPL *session, WT_LSN *ckpt_lsn);
extern void __wt_log_written_reset(WT_SESSION_IMPL *session);
extern void __wt_logmgr_compat_version(WT_SESSION_IMPL *session);
extern void __wt_logrec_free(WT_SESSION_IMPL *session, WT_ITEM **logrecp);
static WT_INLINE int __wt_log_cmp(WT_LSN *lsn1, WT_LSN *lsn2)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE int __wt_lsn_string(WT_LSN *lsn, size_t len, char *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint32_t __wt_lsn_file(WT_LSN *lsn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE uint32_t __wt_lsn_offset(WT_LSN *lsn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));

#ifdef HAVE_UNITTEST

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */
