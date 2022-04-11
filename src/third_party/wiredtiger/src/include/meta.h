/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define WT_WIREDTIGER "WiredTiger"        /* Version file */
#define WT_SINGLETHREAD "WiredTiger.lock" /* Locking file */

#define WT_BASECONFIG "WiredTiger.basecfg"         /* Base configuration */
#define WT_BASECONFIG_SET "WiredTiger.basecfg.set" /* Base config temp */

#define WT_USERCONFIG "WiredTiger.config" /* User configuration */

/*
 * Backup related WiredTiger files.
 */
#define WT_BACKUP_TMP "WiredTiger.backup.tmp"  /* Backup tmp file */
#define WT_METADATA_BACKUP "WiredTiger.backup" /* Hot backup file */
#define WT_LOGINCR_BACKUP "WiredTiger.ibackup" /* Log incremental backup */
#define WT_LOGINCR_SRC "WiredTiger.isrc"       /* Log incremental source */

#define WT_METADATA_TURTLE "WiredTiger.turtle"         /* Metadata metadata */
#define WT_METADATA_TURTLE_SET "WiredTiger.turtle.set" /* Turtle temp file */

#define WT_METADATA_URI "metadata:"           /* Metadata alias */
#define WT_METAFILE "WiredTiger.wt"           /* Metadata table */
#define WT_METAFILE_SLVG "WiredTiger.wt.orig" /* Metadata copy */
#define WT_METAFILE_URI "file:WiredTiger.wt"  /* Metadata table URI */

#define WT_HS_FILE "WiredTigerHS.wt"     /* History store table */
#define WT_HS_URI "file:WiredTigerHS.wt" /* History store table URI */

#define WT_SYSTEM_PREFIX "system:"                               /* System URI prefix */
#define WT_SYSTEM_CKPT_TS "checkpoint_timestamp"                 /* Checkpoint timestamp name */
#define WT_SYSTEM_CKPT_URI "system:checkpoint"                   /* Checkpoint timestamp URI */
#define WT_SYSTEM_OLDEST_TS "oldest_timestamp"                   /* Oldest timestamp name */
#define WT_SYSTEM_OLDEST_URI "system:oldest"                     /* Oldest timestamp URI */
#define WT_SYSTEM_TS_TIME "checkpoint_time"                      /* Checkpoint wall time */
#define WT_SYSTEM_TS_WRITE_GEN "write_gen"                       /* Checkpoint write generation */
#define WT_SYSTEM_CKPT_SNAPSHOT "snapshots"                      /* List of snapshots */
#define WT_SYSTEM_CKPT_SNAPSHOT_MIN "snapshot_min"               /* Snapshot minimum */
#define WT_SYSTEM_CKPT_SNAPSHOT_MAX "snapshot_max"               /* Snapshot maximum */
#define WT_SYSTEM_CKPT_SNAPSHOT_COUNT "snapshot_count"           /* Snapshot count */
#define WT_SYSTEM_CKPT_SNAPSHOT_URI "system:checkpoint_snapshot" /* Checkpoint snapshot URI */
#define WT_SYSTEM_CKPT_SNAPSHOT_TIME "checkpoint_time"           /* Checkpoint wall time */
#define WT_SYSTEM_CKPT_SNAPSHOT_WRITE_GEN "write_gen"            /* Checkpoint write generation */
#define WT_SYSTEM_BASE_WRITE_GEN_URI "system:checkpoint_base_write_gen" /* Base write gen URI */
#define WT_SYSTEM_BASE_WRITE_GEN "base_write_gen"                       /* Base write gen name */

/* Check whether a string is a legal URI for a btree object */
#define WT_BTREE_PREFIX(str) (WT_PREFIX_MATCH(str, "file:") || WT_PREFIX_MATCH(str, "tiered:"))

/*
 * Optimize comparisons against the metafile URI, flag handles that reference the metadata file.
 */
#define WT_IS_METADATA(dh) F_ISSET((dh), WT_DHANDLE_IS_METADATA)
#define WT_METAFILE_ID 0 /* Metadata file ID */

#define WT_METADATA_COMPAT "Compatibility version"
#define WT_METADATA_VERSION "WiredTiger version" /* Version keys */
#define WT_METADATA_VERSION_STR "WiredTiger version string"

/*
 * As a result of a data format change WiredTiger is not able to start on versions below 3.2.0, as
 * it will write out a data format that is not readable by those versions. These version numbers
 * provide such mechanism.
 */
#define WT_MIN_STARTUP_VERSION ((WT_VERSION){3, 2, 0}) /* Minimum version we can start on. */

/*
 * WT_WITH_TURTLE_LOCK --
 *	Acquire the turtle file lock, perform an operation, drop the lock.
 */
#define WT_WITH_TURTLE_LOCK(session, op)                                                      \
    do {                                                                                      \
        WT_ASSERT(session, !FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_TURTLE));        \
        WT_WITH_LOCK_WAIT(session, &S2C(session)->turtle_lock, WT_SESSION_LOCKED_TURTLE, op); \
    } while (0)

/*
 * Block based incremental backup structure. These live in the connection.
 */
#define WT_BLKINCR_MAX 2
struct __wt_blkincr {
    const char *id_str;   /* User's name for this backup. */
    uint64_t granularity; /* Granularity of this backup. */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_BLKINCR_FULL 0x1u  /* There is no checkpoint, always do full file */
#define WT_BLKINCR_INUSE 0x2u /* This entry is active */
#define WT_BLKINCR_VALID 0x4u /* This entry is valid */
                              /* AUTOMATIC FLAG VALUE GENERATION STOP 64 */
    uint64_t flags;
};

/*
 * Block modifications from an incremental identifier going forward.
 */
/*
 * At the default granularity, this is enough for blocks in a 2G file.
 */
#define WT_BLOCK_MODS_LIST_MIN 128 /* Initial bits for bitmap. */
struct __wt_block_mods {
    const char *id_str;

    WT_ITEM bitstring;
    uint64_t nbits; /* Number of bits in bitstring */

    uint64_t offset; /* Zero bit offset for bitstring */
    uint64_t granularity;
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_BLOCK_MODS_RENAME 0x1u /* Entry is from a rename */
#define WT_BLOCK_MODS_VALID 0x2u  /* Entry is valid */
                                  /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * WT_CKPT --
 *	Encapsulation of checkpoint information, shared by the metadata, the
 * btree engine, and the block manager.
 */
#define WT_CHECKPOINT "WiredTigerCheckpoint"
#define WT_CKPT_FOREACH(ckptbase, ckpt) for ((ckpt) = (ckptbase); (ckpt)->name != NULL; ++(ckpt))
#define WT_CKPT_FOREACH_NAME_OR_ORDER(ckptbase, ckpt) \
    for ((ckpt) = (ckptbase); (ckpt)->name != NULL || (ckpt)->order != 0; ++(ckpt))

struct __wt_ckpt {
    char *name; /* Name or NULL */

    /*
     * Each internal checkpoint name is appended with a generation to make it a unique name. We're
     * solving two problems: when two checkpoints are taken quickly, the timer may not be unique
     * and/or we can even see time travel on the second checkpoint if we snapshot the time
     * in-between nanoseconds rolling over. Second, if we reset the generational counter when new
     * checkpoints arrive, we could logically re-create specific checkpoints, racing with cursors
     * open on those checkpoints. I can't think of any way to return incorrect results by racing
     * with those cursors, but it's simpler not to worry about it.
     */
    int64_t order; /* Checkpoint order */

    uint64_t sec; /* Wall clock time */

    uint64_t size; /* Checkpoint size */

    uint64_t write_gen;     /* Write generation */
    uint64_t run_write_gen; /* Runtime write generation. */

    char *block_metadata;   /* Block-stored metadata */
    char *block_checkpoint; /* Block-stored checkpoint */

    WT_BLOCK_MODS backup_blocks[WT_BLKINCR_MAX];

    WT_TIME_AGGREGATE ta; /* Validity window */

    WT_ITEM addr; /* Checkpoint cookie string */
    WT_ITEM raw;  /* Checkpoint cookie raw */

    void *bpriv; /* Block manager private */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CKPT_ADD 0x01u        /* Checkpoint to be added */
#define WT_CKPT_BLOCK_MODS 0x02u /* Return list of modified blocks */
#define WT_CKPT_DELETE 0x04u     /* Checkpoint to be deleted */
#define WT_CKPT_FAKE 0x08u       /* Checkpoint is a fake */
#define WT_CKPT_UPDATE 0x10u     /* Checkpoint requires update */
                                 /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * WT_CKPT_SNAPSHOT --
 *     Snapshot and timestamp information associated with a checkpoint.
 */
struct __wt_ckpt_snapshot {
    uint64_t oldest_ts;
    uint64_t stable_ts;
    uint64_t snapshot_min;
    uint64_t snapshot_max;
    uint64_t *snapshot_txns;
    uint32_t snapshot_count;
};
