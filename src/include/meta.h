/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_METADATA_URI		"file:WiredTiger.wt"

#define	WT_METADATA_TURTLE	"WiredTiger.turtle"	/* Metadata metadata */
#define	WT_METADATA_TURTLE_SET	"WiredTiger.turtle.set"	/* Turtle temp file */

#define	WT_METADATA_VERSION	"WiredTiger version"	/* Version keys */
#define	WT_METADATA_VERSION_STR	"WiredTiger version string"

/*
 * WT_SNAPSHOT --
 *	Encapsulation of snapshot information, shared by the metadata, the
 * btree engine, and the block manager.
 */
#define	WT_INTERNAL_SNAPSHOT	"WiredTigerInternalSnapshot"
#define	WT_SNAPSHOT_FOREACH(snapbase, snap)				\
	for ((snap) = (snapbase); (snap)->name != NULL; ++(snap))

struct __wt_snapshot {
	char	*name;				/* Name or NULL */

	WT_ITEM  addr;				/* Snapshot cookie string */
	WT_ITEM  raw;				/* Snapshot cookie raw */

	int64_t	 order;				/* Snapshot order */

	uintmax_t sec;				/* Timestamp */

	uint64_t snapshot_size;			/* Snapshot size */

	void	*bpriv;				/* Block manager private */

#define	WT_SNAP_ADD	0x01			/* Snapshot to be added */
#define	WT_SNAP_DELETE	0x02			/* Snapshot to be deleted */
#define	WT_SNAP_UPDATE	0x04			/* Snapshot requires update */
	uint32_t flags;
};
