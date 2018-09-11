/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_WIREDTIGER		"WiredTiger"		/* Version file */
#define	WT_SINGLETHREAD		"WiredTiger.lock"	/* Locking file */

#define	WT_BASECONFIG		"WiredTiger.basecfg"	/* Base configuration */
#define	WT_BASECONFIG_SET	"WiredTiger.basecfg.set"/* Base config temp */

#define	WT_USERCONFIG		"WiredTiger.config"	/* User configuration */

#define	WT_BACKUP_TMP		"WiredTiger.backup.tmp"	/* Backup tmp file */
#define	WT_METADATA_BACKUP	"WiredTiger.backup"	/* Hot backup file */
#define	WT_INCREMENTAL_BACKUP	"WiredTiger.ibackup"	/* Incremental backup */
#define	WT_INCREMENTAL_SRC	"WiredTiger.isrc"	/* Incremental source */

#define	WT_METADATA_TURTLE	"WiredTiger.turtle"	/* Metadata metadata */
#define	WT_METADATA_TURTLE_SET	"WiredTiger.turtle.set"	/* Turtle temp file */

#define	WT_METADATA_URI		"metadata:"		/* Metadata alias */
#define	WT_METAFILE		"WiredTiger.wt"		/* Metadata table */
#define	WT_METAFILE_SLVG	"WiredTiger.wt.orig"	/* Metadata copy */
#define	WT_METAFILE_URI		"file:WiredTiger.wt"	/* Metadata table URI */

#define	WT_LAS_URI		"file:WiredTigerLAS.wt"	/* Lookaside table URI*/

#define	WT_SYSTEM_PREFIX	"system:"		/* System URI prefix */
#define	WT_SYSTEM_CKPT_URI	"system:checkpoint"	/* Checkpoint URI */

/*
 * Optimize comparisons against the metafile URI, flag handles that reference
 * the metadata file.
 */
#define	WT_IS_METADATA(dh)      F_ISSET((dh), WT_DHANDLE_IS_METADATA)
#define	WT_METAFILE_ID		0			/* Metadata file ID */

#define	WT_METADATA_COMPAT	"Compatibility version"
#define	WT_METADATA_VERSION	"WiredTiger version"	/* Version keys */
#define	WT_METADATA_VERSION_STR	"WiredTiger version string"

/*
 * WT_WITH_TURTLE_LOCK --
 *	Acquire the turtle file lock, perform an operation, drop the lock.
 */
#define	WT_WITH_TURTLE_LOCK(session, op) do {				\
	WT_ASSERT(session, !F_ISSET(session, WT_SESSION_LOCKED_TURTLE));\
	WT_WITH_LOCK_WAIT(session,					\
	    &S2C(session)->turtle_lock, WT_SESSION_LOCKED_TURTLE, op);	\
} while (0)

/*
 * WT_CKPT --
 *	Encapsulation of checkpoint information, shared by the metadata, the
 * btree engine, and the block manager.
 */
#define	WT_CHECKPOINT		"WiredTigerCheckpoint"
#define	WT_CKPT_FOREACH(ckptbase, ckpt)					\
	for ((ckpt) = (ckptbase); (ckpt)->name != NULL; ++(ckpt))

struct __wt_ckpt {
	char	*name;				/* Name or NULL */

	WT_ITEM  addr;				/* Checkpoint cookie string */
	WT_ITEM  raw;				/* Checkpoint cookie raw */

	int64_t	 order;				/* Checkpoint order */

	uintmax_t sec;				/* Timestamp */

	uint64_t ckpt_size;			/* Checkpoint size */

	uint64_t write_gen;			/* Write generation */

	void	*bpriv;				/* Block manager private */

/* AUTOMATIC FLAG VALUE GENERATION START */
#define	WT_CKPT_ADD	0x1u			/* Checkpoint to be added */
#define	WT_CKPT_DELETE	0x2u			/* Checkpoint to be deleted */
#define	WT_CKPT_FAKE	0x4u			/* Checkpoint is a fake */
#define	WT_CKPT_UPDATE	0x8u			/* Checkpoint requires update */
/* AUTOMATIC FLAG VALUE GENERATION STOP */
	uint32_t flags;
};
