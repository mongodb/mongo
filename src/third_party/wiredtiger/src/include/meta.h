/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_WIREDTIGER		"WiredTiger"		/* Version file */
#define	WT_SINGLETHREAD		"WiredTiger.lock"	/* Locking file */

#define	WT_BASECONFIG		"WiredTiger.basecfg"	/* Configuration */
#define	WT_USERCONFIG		"WiredTiger.config"	/* Configuration */

#define	WT_METADATA_BACKUP	"WiredTiger.backup"	/* Hot backup file */

#define	WT_METADATA_TURTLE	"WiredTiger.turtle"	/* Metadata metadata */
#define	WT_METADATA_TURTLE_SET	"WiredTiger.turtle.set"	/* Turtle temp file */

#define	WT_METADATA_URI		"metadata:"		/* Metadata alias */
#define	WT_METAFILE_URI		"file:WiredTiger.wt"	/* Metadata file URI */
#define	WT_IS_METADATA(dh)						\
	(strcmp((dh)->name, WT_METAFILE_URI) == 0)
#define	WT_METAFILE_ID		0			/* Metadata file ID */

#define	WT_METADATA_VERSION	"WiredTiger version"	/* Version keys */
#define	WT_METADATA_VERSION_STR	"WiredTiger version string"

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

#define	WT_CKPT_ADD	0x01			/* Checkpoint to be added */
#define	WT_CKPT_DELETE	0x02			/* Checkpoint to be deleted */
#define	WT_CKPT_FAKE	0x04			/* Checkpoint is a fake */
#define	WT_CKPT_UPDATE	0x08			/* Checkpoint requires update */
	uint32_t flags;
};
