/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 * All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#define WTI_LOG_PREPNAME "WiredTigerPreplog" /* Log pre-allocated name */
#define WTI_LOG_TMPNAME "WiredTigerTmplog"   /* Log temporary name */

/* Logging subsystem declarations. */
#define WTI_LOG_ALIGN 128

/*
 * Macro to print an LSN.
 */
#define WTI_LSN_MSG(lsn, msg) \
    __wt_msg(session, "%s LSN: [%" PRIu32 "][%" PRIu32 "]", (msg), (lsn)->l.file, (lsn)->l.offset)

/*
 * Both of the macros below need to change if the content of __wt_lsn ever changes. The value is the
 * following: txnid, record type, operation type, file id, operation key, operation value
 */
#define WTI_LOGC_KEY_FORMAT WT_UNCHECKED_STRING(III)
#define WTI_LOGC_VALUE_FORMAT WT_UNCHECKED_STRING(qIIIuu)

#define WTI_LOG_REC_SIZE(size) ((size)-offsetof(WT_LOG_RECORD, record))

/*
 * We allocate the buffer size, but trigger a slot switch when we cross the maximum size of half the
 * buffer. If a record is more than the buffer maximum then we trigger a slot switch and write that
 * record unbuffered. We use a larger buffer to provide overflow space so that we can switch once we
 * cross the threshold.
 */
#define WTI_LOG_SLOT_BUF_SIZE (256 * 1024) /* Must be power of 2 */
#define WTI_LOG_SLOT_BUF_MAX ((uint32_t)log->slot_buf_size / 2)
#define WTI_LOG_SLOT_UNBUFFERED (WTI_LOG_SLOT_BUF_SIZE << 1)

/*
 * Possible values for the consolidation array slot states:
 *
 * WTI_LOG_SLOT_CLOSE - slot is in use but closed to new joins.
 *
 * WTI_LOG_SLOT_FREE - slot is available for allocation.
 *
 * WTI_LOG_SLOT_WRITTEN - slot is written and should be processed by worker.
 *
 * The slot state must be volatile: threads loop checking the state and can't cache the first value
 * they see.
 *
 * The slot state is divided into two 32 bit sizes. One half is the amount joined and the other is
 * the amount released. Since we use a few special states, reserve the top few bits for state. That
 * makes the maximum size less than 32 bits for both joined and released.
 */
/*
 * XXX The log slot bits are signed and should be rewritten as unsigned. For now, give the logging
 * subsystem its own flags macro.
 */
#define FLD_LOG_SLOT_ISSET(field, mask) (((field) & (uint64_t)(mask)) != 0)

/*
 * The high bit is reserved for the special states. If the high bit is set (WTI_LOG_SLOT_RESERVED)
 * then we are guaranteed to be in a special state.
 */
#define WTI_LOG_SLOT_FREE (-1)    /* Not in use */
#define WTI_LOG_SLOT_WRITTEN (-2) /* Slot data written, not processed */

/*
 * If new slot states are added, adjust WTI_LOG_SLOT_BITS and WTI_LOG_SLOT_MASK_OFF accordingly for
 * how much of the top 32 bits we are using. More slot states here will reduce the maximum size that
 * a slot can hold unbuffered by half. If a record is larger than the maximum we can account for in
 * the slot state we fall back to direct writes.
 */
#define WTI_LOG_SLOT_BITS 2
#define WTI_LOG_SLOT_MAXBITS (32 - WTI_LOG_SLOT_BITS)
#define WTI_LOG_SLOT_CLOSE 0x4000000000000000LL    /* Force slot close */
#define WTI_LOG_SLOT_RESERVED 0x8000000000000000LL /* Reserved states */

/*
 * Check if the unbuffered flag is set in the joined portion of the slot state.
 */
#define WTI_LOG_SLOT_UNBUFFERED_ISSET(state) ((state) & ((int64_t)WTI_LOG_SLOT_UNBUFFERED << 32))

#define WTI_LOG_SLOT_MASK_OFF 0x3fffffffffffffffLL
#define WTI_LOG_SLOT_MASK_ON ~(WTI_LOG_SLOT_MASK_OFF)
#define WTI_LOG_SLOT_JOIN_MASK (WTI_LOG_SLOT_MASK_OFF >> 32)

/*
 * These macros manipulate the slot state and its component parts.
 */
#define WTI_LOG_SLOT_FLAGS(state) ((state)&WTI_LOG_SLOT_MASK_ON)
#define WTI_LOG_SLOT_JOINED(state) (((state)&WTI_LOG_SLOT_MASK_OFF) >> 32)
#define WTI_LOG_SLOT_JOINED_BUFFERED(state) \
    (WTI_LOG_SLOT_JOINED(state) & (WTI_LOG_SLOT_UNBUFFERED - 1))
#define WTI_LOG_SLOT_JOIN_REL(j, r, s) (((j) << 32) + (r) + (s))
#define WTI_LOG_SLOT_RELEASED(state) ((int64_t)(int32_t)(state))
#define WTI_LOG_SLOT_RELEASED_BUFFERED(state) \
    ((int64_t)((int32_t)WTI_LOG_SLOT_RELEASED(state) & (WTI_LOG_SLOT_UNBUFFERED - 1)))

/* Slot is in use */
#define WTI_LOG_SLOT_ACTIVE(state) (WTI_LOG_SLOT_JOINED(state) != WTI_LOG_SLOT_JOIN_MASK)
/* Slot is in use, but closed to new joins */
#define WTI_LOG_SLOT_CLOSED(state)                                  \
    (WTI_LOG_SLOT_ACTIVE(state) &&                                  \
      (FLD_LOG_SLOT_ISSET((uint64_t)(state), WTI_LOG_SLOT_CLOSE) && \
        !FLD_LOG_SLOT_ISSET((uint64_t)(state), WTI_LOG_SLOT_RESERVED)))
/* Slot is in use, all data copied into buffer */
#define WTI_LOG_SLOT_INPROGRESS(state) (WTI_LOG_SLOT_RELEASED(state) != WTI_LOG_SLOT_JOINED(state))
#define WTI_LOG_SLOT_DONE(state) (WTI_LOG_SLOT_CLOSED(state) && !WTI_LOG_SLOT_INPROGRESS(state))
/* Slot is in use, more threads may join this slot */
#define WTI_LOG_SLOT_OPEN(state)                                            \
    (WTI_LOG_SLOT_ACTIVE(state) && !WTI_LOG_SLOT_UNBUFFERED_ISSET(state) && \
      !FLD_LOG_SLOT_ISSET((uint64_t)(state), WTI_LOG_SLOT_CLOSE) &&         \
      WTI_LOG_SLOT_JOINED(state) < WTI_LOG_SLOT_BUF_MAX)

struct __wti_logslot {
    WT_CACHE_LINE_PAD_BEGIN
    wt_shared volatile int64_t slot_state; /* Slot state */
    wt_shared int64_t slot_unbuffered;     /* Unbuffered data in this slot */
    wt_shared int slot_error;              /* Error value */
    wt_shared wt_off_t slot_start_offset;  /* Starting file offset */
    wt_shared wt_off_t slot_last_offset;   /* Last record offset */
    WT_LSN slot_release_lsn;               /* Slot release LSN */
    WT_LSN slot_start_lsn;                 /* Slot starting LSN */
    WT_LSN slot_end_lsn;                   /* Slot ending LSN */
    WT_FH *slot_fh;                        /* File handle for this group */
    WT_ITEM slot_buf;                      /* Buffer for grouped writes */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WTI_SLOT_CLOSEFH 0x01u       /* Close old fh on release */
#define WTI_SLOT_FLUSH 0x02u         /* Wait for write */
#define WTI_SLOT_SYNC 0x04u          /* Needs sync on release */
#define WTI_SLOT_SYNC_DIR 0x08u      /* Directory sync on release */
#define WTI_SLOT_SYNC_DIRTY 0x10u    /* Sync system buffers on release */
                                     /* AUTOMATIC FLAG VALUE GENERATION STOP 16 */
    wt_shared uint16_t flags_atomic; /* Atomic flags, use F_*_ATOMIC_16 */
    WT_CACHE_LINE_PAD_END
};

/* Check struct is correctly padded. */
static_assert(sizeof(WTI_LOGSLOT) > WT_CACHE_LINE_ALIGNMENT ||
    sizeof(WTI_LOGSLOT) % WT_CACHE_LINE_ALIGNMENT == 0,
  "WTI_LOGSLOT padding check failed");

#define WTI_SLOT_INIT_FLAGS 0

#define WTI_SLOT_SYNC_FLAGS (WTI_SLOT_SYNC | WTI_SLOT_SYNC_DIR | WTI_SLOT_SYNC_DIRTY)

#define WTI_WITH_SLOT_LOCK(session, log, op)                                           \
    do {                                                                               \
        WT_ASSERT(session, !FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SLOT));   \
        WT_WITH_LOCK_WAIT(session, &(log)->log_slot_lock, WT_SESSION_LOCKED_SLOT, op); \
    } while (0)

struct __wti_myslot {
    WTI_LOGSLOT *slot;   /* Slot I'm using */
    wt_off_t end_offset; /* My end offset in buffer */
    wt_off_t offset;     /* Slot buffer offset */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WTI_MYSLOT_CLOSE 0x1u         /* This thread is closing the slot */
#define WTI_MYSLOT_NEEDS_RELEASE 0x2u /* This thread is releasing the slot */
#define WTI_MYSLOT_UNBUFFERED 0x4u    /* Write directly */
                                      /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

#define WTI_LOG_END_HEADER WTI_LOG_ALIGN

struct __wti_log {
    uint32_t allocsize;            /* Allocation alignment size */
    uint32_t first_record;         /* Offset of first record in file */
    wt_off_t log_written;          /* Amount of log written this period */
                                   /*
                                    * Log file information
                                    */
    uint32_t fileid;               /* Current log file number */
    uint32_t prep_fileid;          /* Pre-allocated file number */
    wt_shared uint32_t tmp_fileid; /* Temporary file number */
#ifdef HAVE_DIAGNOSTIC
    uint32_t min_fileid; /* Minimum file number needed */
#endif
    uint32_t prep_missed;           /* Pre-allocated file misses */
    WT_FH *log_fh;                  /* Logging file handle */
    WT_FH *log_dir_fh;              /* Log directory file handle */
    wt_shared WT_FH *log_close_fh;  /* Logging file handle to close */
    wt_shared WT_LSN log_close_lsn; /* LSN needed to close */

    uint16_t log_version; /* Version of log file */

    /*
     * System LSNs
     */
    WT_LSN alloc_lsn;       /* Next LSN for allocation */
    WT_LSN ckpt_lsn;        /* Last checkpoint LSN */
    WT_LSN dirty_lsn;       /* LSN of last non-synced write */
    WT_LSN first_lsn;       /* First LSN */
    WT_LSN sync_dir_lsn;    /* LSN of the last directory sync */
    WT_LSN sync_lsn;        /* LSN of the last sync */
    WT_LSN trunc_lsn;       /* End LSN for recovery truncation */
    WT_LSN write_lsn;       /* End of last LSN written */
    WT_LSN write_start_lsn; /* Beginning of last LSN written */

    /*
     * Synchronization resources
     */
    WT_SPINLOCK log_lock;          /* Locked: Logging fields */
    WT_SPINLOCK log_fs_lock;       /* Locked: tmp, prep and log files */
    WT_SPINLOCK log_slot_lock;     /* Locked: Consolidation array */
    WT_SPINLOCK log_sync_lock;     /* Locked: Single-thread fsync */
    WT_SPINLOCK log_writelsn_lock; /* Locked: write LSN */

    WT_RWLOCK log_remove_lock; /* Remove and log cursors */

    /* Notify any waiting threads when sync_lsn is updated. */
    WT_CONDVAR *log_sync_cond;
    /* Notify any waiting threads when write_lsn is updated. */
    WT_CONDVAR *log_write_cond;

/*
 * Consolidation array information Our testing shows that the more consolidation we generate the
 * better the performance we see which equates to an active slot count of one.
 *
 * Note: this can't be an array, we impose cache-line alignment and gcc doesn't support that for
 * arrays.
 */
#define WTI_SLOT_POOL 128
    wt_shared WTI_LOGSLOT *active_slot;             /* Active slot */
    wt_shared WTI_LOGSLOT slot_pool[WTI_SLOT_POOL]; /* Pool of all slots */
    int32_t pool_index;                             /* Index into slot pool */
    size_t slot_buf_size;                           /* Buffer size for slots */
#ifdef HAVE_DIAGNOSTIC
    wt_shared uint64_t write_calls; /* Calls to log_write */
#endif

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WTI_LOG_FORCE_NEWFILE 0x1u   /* Force switch to new log file */
#define WTI_LOG_OPENED 0x2u          /* Log subsystem successfully open */
#define WTI_LOG_TRUNCATE_NOTSUP 0x4u /* File system truncate not supported */
                                     /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * WTI_LOG_DESC --
 *	The log file's description.
 */
struct __wti_log_desc {
#define WTI_LOG_MAGIC 0x101064u
    uint32_t log_magic; /* 00-03: Magic number */
                        /*
                         * NOTE: We bumped the log version from 2 to 3 to make it convenient for
                         * MongoDB to detect users accidentally running old binaries on a newer
                         * release. There are no actual log file format changes in versions 2
                         * through 5.
                         */
#define WTI_LOG_VERSION 5
    uint16_t version;  /* 04-05: Log version */
    uint16_t unused;   /* 06-07: Unused */
    uint64_t log_size; /* 08-15: Log file size */
};

/*
 * This is the log version that introduced the system record.
 */
#define WTI_LOG_VERSION_SYSTEM 2

struct __wti_cursor_log {
    WT_CURSOR iface;

    WT_LSN *cur_lsn;                  /* LSN of current record */
    WT_LSN *next_lsn;                 /* LSN of next record */
    WT_ITEM *logrec;                  /* Copy of record for cursor */
    WT_ITEM *opkey, *opvalue;         /* Op key/value copy */
    const uint8_t *stepp, *stepp_end; /* Pointer within record */
    uint8_t *packed_key;              /* Packed key for 'raw' interface */
    uint8_t *packed_value;            /* Packed value for 'raw' interface */
    uint32_t step_count;              /* Intra-record count */
    uint32_t rectype;                 /* Record type */
    uint64_t txnid;                   /* Record txnid */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WTI_CURLOG_REMOVE_LOCK 0x1u /* Remove lock held */
                                    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

extern int __wti_log_acquire(WT_SESSION_IMPL *session, uint64_t recsize, WTI_LOGSLOT *slot)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_allocfile(WT_SESSION_IMPL *session, uint32_t lognum, const char *dest)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_close(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_extract_lognum(WT_SESSION_IMPL *session, const char *name, uint32_t *id)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_fill(WT_SESSION_IMPL *session, WTI_MYSLOT *myslot, bool force, WT_ITEM *record,
  WT_LSN *lsnp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_force_write(WT_SESSION_IMPL *session, bool retry, bool *did_work)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_open(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_recover_prevlsn(WT_SESSION_IMPL *session, const uint8_t **pp,
  const uint8_t *end, WT_LSN *lsnp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_release(WT_SESSION_IMPL *session, WTI_LOGSLOT *slot, bool *freep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_remove(WT_SESSION_IMPL *session, const char *file_prefix, uint32_t lognum)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_set_version(WT_SESSION_IMPL *session, uint16_t version, uint32_t first_rec,
  bool downgrade, bool live_chg, uint32_t *lognump)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_slot_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_slot_init(WT_SESSION_IMPL *session, bool alloc)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_slot_switch(WT_SESSION_IMPL *session, WTI_MYSLOT *myslot, bool retry,
  bool forced, bool *did_work) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_log_system_prevlsn(WT_SESSION_IMPL *session, WT_FH *log_fh, WT_LSN *lsn)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int64_t __wti_log_slot_release(WTI_MYSLOT *myslot, int64_t size)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void __wti_log_slot_activate(WT_SESSION_IMPL *session, WTI_LOGSLOT *slot);
extern void __wti_log_slot_free(WT_SESSION_IMPL *session, WTI_LOGSLOT *slot);
extern void __wti_log_slot_join(
  WT_SESSION_IMPL *session, uint64_t mysize, uint32_t flags, WTI_MYSLOT *myslot);
extern void __wti_log_wrlsn(WT_SESSION_IMPL *session, int *yield);
static WT_INLINE bool __wti_log_is_prealloc_enabled(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE void __wti_log_desc_byteswap(WTI_LOG_DESC *desc);
static WT_INLINE void __wti_log_record_byteswap(WT_LOG_RECORD *record);

#ifdef HAVE_UNITTEST

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */
