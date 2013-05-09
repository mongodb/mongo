/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_LOG_FILENAME	"WiredTigerLog"		/* Log file name */

/* Logging subsystem declarations. */
struct __wt_lsn {
	uint32_t	file;		/* Log file number */
	uint32_t	offset;		/* 128 byte offset */
};

typedef enum {
	WT_LOGREC_INT16,
	WT_LOGREC_UINT16,
	WT_LOGREC_INT32,
	WT_LOGREC_UINT32,
	WT_LOGREC_INT64,
	WT_LOGREC_UINT64,
	WT_LOGREC_STRING,
} WT_LOGREC_FIELDTYPE;

typedef struct {
	const char *fmt;
	const char *fields[];
} WT_LOGREC_DESC;

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
			int32_t	 state;		/* Slot state */
#undef	slot_error
#define	slot_error		u.slot.error
			int32_t	 error;		/* Error value */
#undef	slot_group_size
#define	slot_group_size		u.slot.group_size
			int32_t	 group_size;	/* Group size */
#undef	slot_index
#define	slot_index		u.slot.index
#define	SLOT_INVALID_INDEX	0xffffffff
			uint32_t index;		/* Active slot index */
#undef	slot_start_offset
#define	slot_start_offset	u.slot.start_offset
			off_t	 start_offset;	/* Starting file offset */
#undef	slot_lsn
#define	slot_lsn		u.slot.lsn
			WT_LSN	*lsn;		/* Slot LSN */
#undef	slot_fh
#define	slot_fh			u.slot.fh
			WT_FH	*fh;		/* File handle for this group */
#undef	slot_cond
#define	slot_cond		u.slot.cond
			WT_CONDVAR	*cond;	/* File handle for this group */
#undef	slot_flags
#define	slot_flags		u.slot.flags
#define	SLOT_SYNC	0x01
			uint32_t flags;		/* Flags */
		} slot;
		uint8_t align[128];
	} u;
} WT_LOGSLOT;

typedef struct {
	WT_LOGSLOT	*slot;
	off_t		 offset;
}; WT_MYSLOT;

typedef struct {
	uint32_t	 fileid;	/* Current log file number */
	WT_FH           *log_fh;        /* Logging file handle */

	/*
	 * System LSNs
	 */
	WT_LSN		*alloc_lsn;	/* Next LSN for allocation */
	WT_LSN		*ckpt_lsn;	/* Last checkpoint LSN */
	WT_LSN		*sync_lsn;	/* LSN of the last sync */
	WT_LSN		*write_lsn;	/* Last LSN written to log file */

	/*
	 * Synchronization resources
	 */
	WT_SPINLOCK      log_lock;      /* Locked: Logging fields */
	WT_SPINLOCK      log_slot_lock; /* Locked: Consolidation array */
	WT_CONDVAR      *slot_sync_cond;/* Logging sync wait mutex */

	/*
	 * Consolidation array information
	 */
#define	SLOT_ACTIVE	4
#define	SLOT_POOL	16
	uint32_t	 pool_index;		/* Global pool index */
	WT_LOGSLOT	*slot_array[SLOT_ACTIVE];	/* Active slots */
	WT_LOGSLOT	 slot_pool[SLOT_POOL];	/* Pool of all slots */

#define	LOG_AUTOREMOVE		0x0001
#define	LOG_RECOVER		0x0002
	uint32_t	 flags;
} WT_LOG;

typedef struct {

} WT_LOG_HDR;

#define	WT_LOG_MAGIC	0x101064
typedef struct {
	uint32_t	log_magic;
} WT_LOGFILE_HDR;
