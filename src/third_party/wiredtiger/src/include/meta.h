/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#define WT_WIREDTIGER "WiredTiger"        /* Version file */
#define WT_SINGLETHREAD "WiredTiger.lock" /* Locking file */

#define WT_BASECONFIG "WiredTiger.basecfg"         /* Base configuration */
#define WT_BASECONFIG_SET "WiredTiger.basecfg.set" /* Base config temp */

#define WT_USERCONFIG "WiredTiger.config" /* User configuration */

/*
 * Backup related WiredTiger files.
 */
#define WT_BACKUP_TMP "WiredTiger.backup.tmp"  /* Backup tmp file */
#define WT_EXPORT_BACKUP "WiredTiger.export"   /* Export backup file */
#define WT_METADATA_BACKUP "WiredTiger.backup" /* Hot backup file */

#define WT_METADATA_TURTLE "WiredTiger.turtle"         /* Metadata metadata */
#define WT_METADATA_TURTLE_SET "WiredTiger.turtle.set" /* Turtle temp file */

#define WT_METADATA_URI "metadata:"           /* Metadata alias */
#define WT_METAFILE "WiredTiger.wt"           /* Metadata table */
#define WT_METAFILE_SLVG "WiredTiger.wt.orig" /* Metadata copy */
#define WT_METAFILE_URI "file:WiredTiger.wt"  /* Metadata table URI */

#define WT_HS_FILE "WiredTigerHS.wt"                         /* History store table */
#define WT_HS_FILE_SHARED "WiredTigerSharedHS.wt_stable"     /* Shared history store */
#define WT_HS_URI "file:WiredTigerHS.wt"                     /* History store table URI */
#define WT_HS_URI_SHARED "file:WiredTigerSharedHS.wt_stable" /* Shared history store URI */
#define WT_HS_ID 1                                           /* ID for HS */
#define WT_HS_ID_SHARED 2                                    /* ID for shared HS */

#define WT_CC_METAFILE "WiredTigerCC.wt"          /* Chunk cache metadata table */
#define WT_CC_METAFILE_URI "file:WiredTigerCC.wt" /* Chunk cache metadata table URI */

#define WT_DISAGG_METADATA_FILE "WiredTigerShared.wt_stable"     /* Shared metadata table */
#define WT_DISAGG_METADATA_URI "file:WiredTigerShared.wt_stable" /* Shared metadata table URI */
#define WT_DISAGG_METADATA_TABLE_ID 1                            /* Table ID for metadata strings */
#define WT_DISAGG_METADATA_MAIN_PAGE_ID 1                        /* Page ID for the main metadata */

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
#define WT_METADATA_LIVE_RESTORE "Live Restore"
#define WT_METADATA_VERSION "WiredTiger version" /* Version keys */
#define WT_METADATA_VERSION_STR "WiredTiger version string"

/*
 * Other useful comparisons.
 */
#define WT_IS_URI_HS(uri) (strcmp(uri, WT_HS_URI) == 0 || strcmp(uri, WT_HS_URI_SHARED) == 0)

#define WT_HS_ID_TO_URI(session, hs_id, uri)                                                   \
    do {                                                                                       \
        switch ((hs_id)) {                                                                     \
        case WT_HS_ID:                                                                         \
            (uri) = WT_HS_URI;                                                                 \
            break;                                                                             \
        case WT_HS_ID_SHARED:                                                                  \
            (uri) = WT_HS_URI_SHARED;                                                          \
            break;                                                                             \
        default:                                                                               \
            WT_ASSERT_ALWAYS((session), false, "No such History Store ID: %" PRIu32, (hs_id)); \
        }                                                                                      \
    } while (0)

#define WT_IS_URI_METADATA(uri) \
    (strcmp(uri, WT_METAFILE_URI) == 0 || strcmp(uri, WT_DISAGG_METADATA_URI) == 0)

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
struct __wt_blkincr {
    const char *id_str;   /* User's name for this backup. */
    uint64_t granularity; /* Granularity of this backup. */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_BLKINCR_FULL 0x1u  /* There is no checkpoint, always do full file */
#define WT_BLKINCR_INUSE 0x2u /* This entry is active */
#define WT_BLKINCR_VALID 0x4u /* This entry is valid */
                              /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};
