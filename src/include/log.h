/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_LOG_FILENAME	"WiredTigerLog"		/* Log file name */
#define	WT_LOG_PREPNAME	"WiredTigerPreplog"	/* Log pre-allocated name */
#define	WT_LOG_TMPNAME	"WiredTigerTmplog"	/* Log temporary name */

/* Logging subsystem declarations. */
#define	WT_LOG_ALIGN			128
#define	WT_LOG_SLOT_BUF_SIZE		256 * 1024

#define	WT_INIT_LSN(l)	do {						\
	(l)->file = 1;							\
	(l)->offset = 0;						\
} while (0)

#define	WT_MAX_LSN(l)	do {						\
	(l)->file = UINT32_MAX;						\
	(l)->offset = INT64_MAX;					\
} while (0)

#define	WT_ZERO_LSN(l)	do {						\
	(l)->file = 0;							\
	(l)->offset = 0;						\
} while (0)

#define	WT_IS_INIT_LSN(l)						\
	((l)->file == 1 && (l)->offset == 0)
#define	WT_IS_MAX_LSN(l)						\
	((l)->file == UINT32_MAX && (l)->offset == INT64_MAX)

/*
 * Both of the macros below need to change if the content of __wt_lsn
 * ever changes.  The value is the following:
 * txnid, record type, operation type, file id, operation key, operation value
 */
#define	WT_LOGC_KEY_FORMAT	WT_UNCHECKED_STRING(IqI)
#define	WT_LOGC_VALUE_FORMAT	WT_UNCHECKED_STRING(qIIIuu)

#define	WT_LOG_SKIP_HEADER(data)					\
    ((const uint8_t *)(data) + offsetof(WT_LOG_RECORD, record))
#define	WT_LOG_REC_SIZE(size)						\
    ((size) - offsetof(WT_LOG_RECORD, record))

/*
 * Compare 2 LSNs, return -1 if lsn0 < lsn1, 0 if lsn0 == lsn1
 * and 1 if lsn0 > lsn1.
 */
#define	WT_LOG_CMP(lsn1, lsn2)						\
	((lsn1)->file != (lsn2)->file ?					\
	((lsn1)->file < (lsn2)->file ? -1 : 1) :			\
	((lsn1)->offset != (lsn2)->offset ?				\
	((lsn1)->offset < (lsn2)->offset ? -1 : 1) : 0))

/*
 * Possible values for the consolidation array slot states:
 * (NOTE: Any new states must be > WT_LOG_SLOT_DONE and < WT_LOG_SLOT_READY.)
 *
 * < WT_LOG_SLOT_DONE - threads are actively writing to the log.
 * WT_LOG_SLOT_DONE - all activity on this slot is complete.
 * WT_LOG_SLOT_FREE - slot is available for allocation.
 * WT_LOG_SLOT_PENDING - slot is transitioning from ready to active.
 * WT_LOG_SLOT_WRITTEN - slot is written and should be processed by worker.
 * WT_LOG_SLOT_READY - slot is ready for threads to join.
 * > WT_LOG_SLOT_READY - threads are actively consolidating on this slot.
 *
 * The slot state must be volatile: threads loop checking the state and can't
 * cache the first value they see.
 */
#define	WT_LOG_SLOT_DONE	0
#define	WT_LOG_SLOT_FREE	1
#define	WT_LOG_SLOT_PENDING	2
#define	WT_LOG_SLOT_WRITTEN	3
#define	WT_LOG_SLOT_READY	4
typedef WT_COMPILER_TYPE_ALIGN(WT_CACHE_LINE_ALIGNMENT) struct {
	volatile int64_t slot_state;	/* Slot state */
	uint64_t slot_group_size;	/* Group size */
	int32_t	 slot_error;		/* Error value */
#define	WT_SLOT_INVALID_INDEX	0xffffffff
	uint32_t slot_index;		/* Active slot index */
	wt_off_t slot_start_offset;	/* Starting file offset */
	WT_LSN	slot_release_lsn;	/* Slot release LSN */
	WT_LSN	slot_start_lsn;		/* Slot starting LSN */
	WT_LSN	slot_end_lsn;		/* Slot ending LSN */
	WT_FH	*slot_fh;		/* File handle for this group */
	WT_ITEM slot_buf;		/* Buffer for grouped writes */
	int32_t	slot_churn;		/* Active slots are scarce. */

#define	WT_SLOT_BUFFERED	0x01		/* Buffer writes */
#define	WT_SLOT_CLOSEFH		0x02		/* Close old fh on release */
#define	WT_SLOT_SYNC		0x04		/* Needs sync on release */
#define	WT_SLOT_SYNC_DIR	0x08		/* Directory sync on release */
	uint32_t flags;			/* Flags */
} WT_LOGSLOT;

#define	WT_SLOT_INIT_FLAGS	(WT_SLOT_BUFFERED)

typedef struct {
	WT_LOGSLOT	*slot;
	wt_off_t	 offset;
} WT_MYSLOT;

					/* Offset of first record */
#define	WT_LOG_FIRST_RECORD	log->allocsize

typedef struct {
	uint32_t	allocsize;	/* Allocation alignment size */
	wt_off_t	log_written;	/* Amount of log written this period */
	/*
	 * Log file information
	 */
	uint32_t	 fileid;	/* Current log file number */
	uint32_t	 prep_fileid;	/* Pre-allocated file number */
	uint32_t	 tmp_fileid;	/* Temporary file number */
	uint32_t	 prep_missed;	/* Pre-allocated file misses */
	WT_FH           *log_fh;	/* Logging file handle */
	WT_FH           *log_close_fh;	/* Logging file handle to close */
	WT_FH           *log_dir_fh;	/* Log directory file handle */

	/*
	 * System LSNs
	 */
	WT_LSN		alloc_lsn;	/* Next LSN for allocation */
	WT_LSN		bg_sync_lsn;	/* Latest background sync LSN */
	WT_LSN		ckpt_lsn;	/* Last checkpoint LSN */
	WT_LSN		first_lsn;	/* First LSN */
	WT_LSN		sync_dir_lsn;	/* LSN of the last directory sync */
	WT_LSN		sync_lsn;	/* LSN of the last sync */
	WT_LSN		trunc_lsn;	/* End LSN for recovery truncation */
	WT_LSN		write_lsn;	/* End of last LSN written */
	WT_LSN		write_start_lsn;/* Beginning of last LSN written */

	/*
	 * Synchronization resources
	 */
	WT_SPINLOCK      log_lock;      /* Locked: Logging fields */
	WT_SPINLOCK      log_slot_lock; /* Locked: Consolidation array */
	WT_SPINLOCK      log_sync_lock; /* Locked: Single-thread fsync */

	WT_RWLOCK	 *log_archive_lock; /* Archive and log cursors */

	/* Notify any waiting threads when sync_lsn is updated. */
	WT_CONDVAR	*log_sync_cond;
	/* Notify any waiting threads when write_lsn is updated. */
	WT_CONDVAR	*log_write_cond;

	/*
	 * Consolidation array information
	 * WT_SLOT_ACTIVE must be less than WT_SLOT_POOL.
	 * Our testing shows that the more consolidation we generate the
	 * better the performance we see which equates to an active slot
	 * slot count of one.
	 */
#define	WT_SLOT_ACTIVE	1
#define	WT_SLOT_POOL	128
	WT_LOGSLOT	*slot_array[WT_SLOT_ACTIVE];	/* Active slots */
	WT_LOGSLOT	 slot_pool[WT_SLOT_POOL];	/* Pool of all slots */
	size_t		 slot_buf_size;		/* Buffer size for slots */

#define	WT_LOG_FORCE_CONSOLIDATE	0x01	/* Disable direct writes */
	uint32_t	 flags;
} WT_LOG;

typedef struct {
	uint32_t	len;		/* 00-03: Record length including hdr */
	uint32_t	checksum;	/* 04-07: Checksum of the record */

#define	WT_LOG_RECORD_COMPRESSED	0x01	/* Compressed except hdr */
#define	WT_LOG_RECORD_ENCRYPTED		0x02	/* Encrypted except hdr */
	uint16_t	flags;		/* 08-09: Flags */
	uint8_t		unused[2];	/* 10-11: Padding */
	uint32_t	mem_len;	/* 12-15: Uncompressed len if needed */
	uint8_t		record[0];	/* Beginning of actual data */
} WT_LOG_RECORD;

/*
 * WT_LOG_DESC --
 *	The log file's description.
 */
struct __wt_log_desc {
#define	WT_LOG_MAGIC		0x101064
	uint32_t	log_magic;	/* 00-03: Magic number */
#define	WT_LOG_MAJOR_VERSION	1
	uint16_t	majorv;		/* 04-05: Major version */
#define	WT_LOG_MINOR_VERSION	0
	uint16_t	minorv;		/* 06-07: Minor version */
	uint64_t	log_size;	/* 08-15: Log file size */
};

/*
 * WT_LOG_REC_DESC --
 *	A descriptor for a log record type.
 */
struct __wt_log_rec_desc {
	const char *fmt;
	int (*print)(WT_SESSION_IMPL *session, uint8_t **pp, uint8_t *end);
};

/*
 * WT_LOG_OP_DESC --
 *	A descriptor for a log operation type.
 */
struct __wt_log_op_desc {
	const char *fmt;
	int (*print)(WT_SESSION_IMPL *session, uint8_t **pp, uint8_t *end);
};
