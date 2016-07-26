/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_LSN --
 *	A log sequence number, representing a position in the transaction log.
 */
union __wt_lsn {
	struct {
#ifdef	WORDS_BIGENDIAN
		uint32_t file;
		uint32_t offset;
#else
		uint32_t offset;
		uint32_t file;
#endif
	} l;
	uint64_t file_offset;
};

#define	WT_LOG_FILENAME	"WiredTigerLog"		/* Log file name */
#define	WT_LOG_PREPNAME	"WiredTigerPreplog"	/* Log pre-allocated name */
#define	WT_LOG_TMPNAME	"WiredTigerTmplog"	/* Log temporary name */

/* Logging subsystem declarations. */
#define	WT_LOG_ALIGN			128

/*
 * Atomically set the two components of the LSN.
 */
#define	WT_SET_LSN(l, f, o) (l)->file_offset = (((uint64_t)(f) << 32) + (o))

#define	WT_INIT_LSN(l)	WT_SET_LSN((l), 1, 0)

#define	WT_MAX_LSN(l)	WT_SET_LSN((l), UINT32_MAX, INT32_MAX)

#define	WT_ZERO_LSN(l)	WT_SET_LSN((l), 0, 0)

/*
 * Initialize LSN is (1,0).  We only need to shift the 1 for comparison.
 */
#define	WT_IS_INIT_LSN(l)	((l)->file_offset == ((uint64_t)1 << 32))
/*
 * Original tested INT32_MAX.  But if we read one from an older
 * release we may see UINT32_MAX.
 */
#define	WT_IS_MAX_LSN(lsn)						\
	((lsn)->l.file == UINT32_MAX &&					\
	 ((lsn)->l.offset == INT32_MAX || (lsn)->l.offset == UINT32_MAX))

/*
 * Both of the macros below need to change if the content of __wt_lsn
 * ever changes.  The value is the following:
 * txnid, record type, operation type, file id, operation key, operation value
 */
#define	WT_LOGC_KEY_FORMAT	WT_UNCHECKED_STRING(III)
#define	WT_LOGC_VALUE_FORMAT	WT_UNCHECKED_STRING(qIIIuu)

#define	WT_LOG_SKIP_HEADER(data)					\
    ((const uint8_t *)(data) + offsetof(WT_LOG_RECORD, record))
#define	WT_LOG_REC_SIZE(size)						\
    ((size) - offsetof(WT_LOG_RECORD, record))

/*
 * Possible values for the consolidation array slot states:
 *
 * WT_LOG_SLOT_CLOSE - slot is in use but closed to new joins.
 * WT_LOG_SLOT_FREE - slot is available for allocation.
 * WT_LOG_SLOT_WRITTEN - slot is written and should be processed by worker.
 *
 * The slot state must be volatile: threads loop checking the state and can't
 * cache the first value they see.
 *
 * The slot state is divided into two 32 bit sizes.  One half is the
 * amount joined and the other is the amount released.  Since we use
 * a few special states, reserve the top few bits for state.  That makes
 * the maximum size less than 32 bits for both joined and released.
 */

/*
 * The high bit is reserved for the special states.  If the high bit is
 * set (WT_LOG_SLOT_RESERVED) then we are guaranteed to be in a special state.
 */
#define	WT_LOG_SLOT_FREE	-1	/* Not in use */
#define	WT_LOG_SLOT_WRITTEN	-2	/* Slot data written, not processed */

/*
 * We allocate the buffer size, but trigger a slot switch when we cross
 * the maximum size of half the buffer.  If a record is more than the buffer
 * maximum then we trigger a slot switch and write that record unbuffered.
 * We use a larger buffer to provide overflow space so that we can switch
 * once we cross the threshold.
 */
#define	WT_LOG_SLOT_BUF_SIZE		(256 * 1024)	/* Must be power of 2 */
#define	WT_LOG_SLOT_BUF_MAX		((uint32_t)log->slot_buf_size / 2)
#define	WT_LOG_SLOT_UNBUFFERED		(WT_LOG_SLOT_BUF_SIZE << 1)

/*
 * If new slot states are added, adjust WT_LOG_SLOT_BITS and
 * WT_LOG_SLOT_MASK_OFF accordingly for how much of the top 32
 * bits we are using.  More slot states here will reduce the maximum
 * size that a slot can hold unbuffered by half.  If a record is
 * larger than the maximum we can account for in the slot state we fall
 * back to direct writes.
 */
#define	WT_LOG_SLOT_BITS	2
#define	WT_LOG_SLOT_MAXBITS	(32 - WT_LOG_SLOT_BITS)
#define	WT_LOG_SLOT_CLOSE	0x4000000000000000LL	/* Force slot close */
#define	WT_LOG_SLOT_RESERVED	0x8000000000000000LL	/* Reserved states */

/*
 * Check if the unbuffered flag is set in the joined portion of
 * the slot state.
 */
#define	WT_LOG_SLOT_UNBUFFERED_ISSET(state)				\
    ((state) & ((int64_t)WT_LOG_SLOT_UNBUFFERED << 32))

#define	WT_LOG_SLOT_MASK_OFF	0x3fffffffffffffffLL
#define	WT_LOG_SLOT_MASK_ON	~(WT_LOG_SLOT_MASK_OFF)
#define	WT_LOG_SLOT_JOIN_MASK	(WT_LOG_SLOT_MASK_OFF >> 32)

/*
 * These macros manipulate the slot state and its component parts.
 */
#define	WT_LOG_SLOT_FLAGS(state)	((state) & WT_LOG_SLOT_MASK_ON)
#define	WT_LOG_SLOT_JOINED(state)	(((state) & WT_LOG_SLOT_MASK_OFF) >> 32)
#define	WT_LOG_SLOT_JOINED_BUFFERED(state)				\
    (WT_LOG_SLOT_JOINED(state) &			\
    (WT_LOG_SLOT_UNBUFFERED - 1))
#define	WT_LOG_SLOT_JOIN_REL(j, r, s)	(((j) << 32) + (r) + (s))
#define	WT_LOG_SLOT_RELEASED(state)	((int64_t)(int32_t)(state))
#define	WT_LOG_SLOT_RELEASED_BUFFERED(state)				\
    ((int64_t)((int32_t)WT_LOG_SLOT_RELEASED(state) &			\
    (WT_LOG_SLOT_UNBUFFERED - 1)))

/* Slot is in use */
#define	WT_LOG_SLOT_ACTIVE(state)					\
    (WT_LOG_SLOT_JOINED(state) != WT_LOG_SLOT_JOIN_MASK)
/* Slot is in use, but closed to new joins */
#define	WT_LOG_SLOT_CLOSED(state)					\
    (WT_LOG_SLOT_ACTIVE(state) &&					\
    (FLD64_ISSET((uint64_t)state, WT_LOG_SLOT_CLOSE) &&			\
    !FLD64_ISSET((uint64_t)state, WT_LOG_SLOT_RESERVED)))
/* Slot is in use, all data copied into buffer */
#define	WT_LOG_SLOT_INPROGRESS(state)					\
    (WT_LOG_SLOT_RELEASED(state) != WT_LOG_SLOT_JOINED(state))
#define	WT_LOG_SLOT_DONE(state)						\
    (WT_LOG_SLOT_CLOSED(state) &&					\
    !WT_LOG_SLOT_INPROGRESS(state))
/* Slot is in use, more threads may join this slot */
#define	WT_LOG_SLOT_OPEN(state)						\
    (WT_LOG_SLOT_ACTIVE(state) &&					\
    !WT_LOG_SLOT_UNBUFFERED_ISSET(state) &&				\
    !FLD64_ISSET((uint64_t)(state), WT_LOG_SLOT_CLOSE) &&		\
    WT_LOG_SLOT_JOINED(state) < WT_LOG_SLOT_BUF_MAX)

struct WT_COMPILER_TYPE_ALIGN(WT_CACHE_LINE_ALIGNMENT) __wt_logslot {
	volatile int64_t slot_state;	/* Slot state */
	int64_t	 slot_unbuffered;	/* Unbuffered data in this slot */
	int32_t	 slot_error;		/* Error value */
	wt_off_t slot_start_offset;	/* Starting file offset */
	wt_off_t slot_last_offset;	/* Last record offset */
	WT_LSN	 slot_release_lsn;	/* Slot release LSN */
	WT_LSN	 slot_start_lsn;	/* Slot starting LSN */
	WT_LSN	 slot_end_lsn;		/* Slot ending LSN */
	WT_FH	*slot_fh;		/* File handle for this group */
	WT_ITEM  slot_buf;		/* Buffer for grouped writes */

#define	WT_SLOT_CLOSEFH		0x01		/* Close old fh on release */
#define	WT_SLOT_FLUSH		0x02		/* Wait for write */
#define	WT_SLOT_SYNC		0x04		/* Needs sync on release */
#define	WT_SLOT_SYNC_DIR	0x08		/* Directory sync on release */
	uint32_t flags;			/* Flags */
};

#define	WT_SLOT_INIT_FLAGS	0

#define	WT_WITH_SLOT_LOCK(session, log, ret, op) do {			\
	WT_ASSERT(session, !F_ISSET(session, WT_SESSION_LOCKED_SLOT));	\
	WT_WITH_LOCK(session, ret,					\
	    &log->log_slot_lock, WT_SESSION_LOCKED_SLOT, op);		\
} while (0)

struct __wt_myslot {
	WT_LOGSLOT	*slot;		/* Slot I'm using */
	wt_off_t	 end_offset;	/* My end offset in buffer */
	wt_off_t	 offset;	/* Slot buffer offset */
#define	WT_MYSLOT_CLOSE		0x01	/* This thread is closing the slot */
#define	WT_MYSLOT_UNBUFFERED	0x02	/* Write directly */
	uint32_t flags;			/* Flags */
};

#define	WT_LOG_FIRST_RECORD	log->allocsize

struct __wt_log {
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
	WT_FH           *log_dir_fh;	/* Log directory file handle */
	WT_FH           *log_close_fh;	/* Logging file handle to close */
	WT_LSN		 log_close_lsn;	/* LSN needed to close */

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
	WT_SPINLOCK      log_writelsn_lock; /* Locked: write LSN */

	WT_RWLOCK	 *log_archive_lock;	/* Archive and log cursors */

	/* Notify any waiting threads when sync_lsn is updated. */
	WT_CONDVAR	*log_sync_cond;
	/* Notify any waiting threads when write_lsn is updated. */
	WT_CONDVAR	*log_write_cond;

	/*
	 * Consolidation array information
	 * Our testing shows that the more consolidation we generate the
	 * better the performance we see which equates to an active slot
	 * slot count of one.
	 *
	 * Note: this can't be an array, we impose cache-line alignment and
	 * gcc doesn't support that for arrays.
	 */
#define	WT_SLOT_POOL	128
	WT_LOGSLOT	*active_slot;			/* Active slot */
	WT_LOGSLOT	 slot_pool[WT_SLOT_POOL];	/* Pool of all slots */
	size_t		 slot_buf_size;		/* Buffer size for slots */
#ifdef HAVE_DIAGNOSTIC
	uint64_t	 write_calls;		/* Calls to log_write */
#endif
#define	WT_LOG_OPENED	0x01		/* Log subsystem successfully open */
	uint32_t	flags;
};

struct __wt_log_record {
	uint32_t	len;		/* 00-03: Record length including hdr */
	uint32_t	checksum;	/* 04-07: Checksum of the record */

#define	WT_LOG_RECORD_COMPRESSED	0x01	/* Compressed except hdr */
#define	WT_LOG_RECORD_ENCRYPTED		0x02	/* Encrypted except hdr */
	uint16_t	flags;		/* 08-09: Flags */
	uint8_t		unused[2];	/* 10-11: Padding */
	uint32_t	mem_len;	/* 12-15: Uncompressed len if needed */
	uint8_t		record[0];	/* Beginning of actual data */
};

/*
 * __wt_log_record_byteswap --
 *	Handle big- and little-endian transformation of the log record
 *	header block.
 */
static inline void
__wt_log_record_byteswap(WT_LOG_RECORD *record)
{
#ifdef	WORDS_BIGENDIAN
	record->len = __wt_bswap32(record->len);
	record->checksum = __wt_bswap32(record->checksum);
	record->flags = __wt_bswap16(record->flags);
	record->mem_len = __wt_bswap32(record->mem_len);
#else
	WT_UNUSED(record);
#endif
}

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
 * __wt_log_desc_byteswap --
 *	Handle big- and little-endian transformation of the log file
 *	description block.
 */
static inline void
__wt_log_desc_byteswap(WT_LOG_DESC *desc)
{
#ifdef	WORDS_BIGENDIAN
	desc->log_magic = __wt_bswap32(desc->log_magic);
	desc->majorv = __wt_bswap16(desc->majorv);
	desc->minorv = __wt_bswap16(desc->minorv);
	desc->log_size = __wt_bswap64(desc->log_size);
#else
	WT_UNUSED(desc);
#endif
}

/*
 * Flags for __wt_txn_op_printlog.
 */
#define	WT_TXN_PRINTLOG_HEX	0x0001	/* Add hex output */

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
