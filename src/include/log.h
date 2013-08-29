/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_LOG_FILENAME	"WiredTigerLog"		/* Log file name */

/* Logging subsystem declarations. */
#define	LOG_ALIGN		128

struct __wt_lsn {
	uint32_t	file;		/* Log file number */
	uint32_t	unused;
	off_t		offset;		/* Log file offset */
};

#define	INIT_LSN(l)	do {						\
	(l)->file = 1;							\
	(l)->offset = 0;						\
} while (0)

#define	MAX_LSN(l)	do {						\
	(l)->file = UINT32_MAX;						\
	(l)->offset = INT64_MAX;					\
} while (0)

/*
 * Compare 2 LSNs, return -1 if lsn0 < lsn1, 0 if lsn0 == lsn1
 * and 1 if lsn0 > lsn1.
 */
#define	LOG_CMP(lsn1, lsn2)						\
	((lsn1)->file != (lsn2)->file ?                                 \
	((lsn1)->file < (lsn2)->file ? -1 : 1) :                        \
	((lsn1)->offset != (lsn2)->offset ?                             \
	((lsn1)->offset < (lsn2)->offset ? -1 : 1) : 0))

/*
 * Possible values for the consolidation array slot states:
 * < WT_LOG_SLOT_DONE - threads are actively writing to the log.
 * WT_LOG_SLOT_DONE - all activity on this slot is complete.
 * WT_LOG_SLOT_FREE - slot is available for allocation.
 * WT_LOG_SLOT_PENDING - slot is transitioning from ready to active.
 * WT_LOG_SLOT_READY - slot is ready for threads to join.
 * > WT_LOG_SLOT_READY - threads are actively consolidating on this slot.
 */
#define	WT_LOG_SLOT_DONE	0
#define	WT_LOG_SLOT_FREE	1
#define	WT_LOG_SLOT_PENDING	2
#define	WT_LOG_SLOT_READY	3
typedef struct {
	union {
		struct {
#undef	slot_state
#define	slot_state		u.slot.state
			int64_t	 state;		/* Slot state */
#undef	slot_group_size
#define	slot_group_size		u.slot.group_size
			uint64_t group_size;	/* Group size */
#undef	slot_error
#define	slot_error		u.slot.error
			int32_t	 error;		/* Error value */
#undef	slot_index
#define	slot_index		u.slot.index
#define	SLOT_INVALID_INDEX	0xffffffff
			uint32_t index;		/* Active slot index */
#undef	slot_start_offset
#define	slot_start_offset	u.slot.start_offset
			off_t	 start_offset;	/* Starting file offset */
#undef	slot_release_lsn
#define	slot_release_lsn	u.slot.release_lsn
			WT_LSN	release_lsn;	/* Slot release LSN */
#undef	slot_start_lsn
#define	slot_start_lsn		u.slot.start_lsn
			WT_LSN	start_lsn;	/* Slot starting LSN */
#undef	slot_end_lsn
#define	slot_end_lsn		u.slot.end_lsn
			WT_LSN	end_lsn;	/* Slot ending LSN */
#undef	slot_fh
#define	slot_fh			u.slot.fh
			WT_FH	*fh;		/* File handle for this group */
#undef	slot_flags
#define	slot_flags		u.slot.flags
#define	SLOT_CLOSEFH	0x01			/* Close old fh on release */
#define	SLOT_SYNC	0x02			/* Needs sync on release */
			uint32_t flags;		/* Flags */
		} slot;
		uint8_t align[LOG_ALIGN];
	} u;
} WT_LOGSLOT;

typedef struct {
	WT_LOGSLOT	*slot;
	off_t		 offset;
} WT_MYSLOT;

#define	LOG_FIRST_RECORD	log->allocsize	/* Offset of first record */

typedef struct {
	uint32_t	allocsize;	/* Allocation alignment size */
	/*
	 * Log file information
	 */
	uint32_t	 fileid;	/* Current log file number */
	WT_FH           *log_fh;	/* Logging file handle */
	WT_FH           *log_close_fh;	/* Logging file handle to close */

	/*
	 * System LSNs
	 */
	WT_LSN		alloc_lsn;	/* Next LSN for allocation */
	WT_LSN		ckpt_lsn;	/* Last checkpoint LSN */
	WT_LSN		first_lsn;	/* First LSN */
	WT_LSN		sync_lsn;	/* LSN of the last sync */
	WT_LSN		write_lsn;	/* Last LSN written to log file */

	/*
	 * Synchronization resources
	 */
	WT_SPINLOCK      log_lock;      /* Locked: Logging fields */
	WT_SPINLOCK      log_slot_lock; /* Locked: Consolidation array */

	/*
	 * Consolidation array information
	 * SLOT_ACTIVE must be less than SLOT_POOL.
	 */
#define	SLOT_ACTIVE	4
#define	SLOT_POOL	16
	uint32_t	 pool_index;		/* Global pool index */
	WT_LOGSLOT	*slot_array[SLOT_ACTIVE];	/* Active slots */
	WT_LOGSLOT	 slot_pool[SLOT_POOL];	/* Pool of all slots */

	uint32_t	 flags;			/* Currently unused */
} WT_LOG;

typedef struct {
	uint32_t	len;		/* 00-03: Record length including hdr */
	uint32_t	checksum;	/* 04-07: Checksum of the record */
	uint8_t		unused[8];	/* 08-15: Padding */
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

/*
 * DO NOT EDIT: automatically built by dist/log.py.
 * Log record declarations: BEGIN
 */
#define	WT_LOGREC_INVALID	0
#define	WT_LOGREC_CHECKPOINT	1
#define	WT_LOGREC_COMMIT	2
#define	WT_LOGREC_DEBUG	3
#define	WT_LOGREC_INVALID	0
#define	WT_LOGOP_COL_PUT	1
#define	WT_LOGOP_COL_REMOVE	2
#define	WT_LOGOP_ROW_PUT	3
#define	WT_LOGOP_ROW_REMOVE	4
/*
 * Log record declarations: END
 * DO NOT EDIT: automatically built by dist/log.py.
 */
