/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#define WT_LIVE_RESTORE_FS_TOMBSTONE_SUFFIX ".deleted"

/*
 * WT_OFFSET_END returns the last byte used by a range (inclusive). i.e. if we have an offset=0 and
 * length=1024 WT_OFFSET_END returns 1023
 */
#define WT_OFFSET_END(offset, len) (offset + (wt_off_t)len - 1)
#define WT_EXTENT_END(ext) WT_OFFSET_END((ext)->off, (ext)->len)
/* As extent ranges are inclusive we want >= and <= on both ends of the range. */
#define WT_OFFSET_IN_EXTENT(addr, ext) ((addr) >= (ext)->off && (addr) <= WT_EXTENT_END(ext))

/*
 * __wt_live_restore_hole_node --
 *     A linked list of extents. Each extent represents a hole in the destination file that needs to
 *     be read from the source file.
 */
struct __wt_live_restore_hole_node {
    wt_off_t off;
    size_t len;

    WT_LIVE_RESTORE_HOLE_NODE *next;
};

/*
 * WT_LIVE_RESTORE_DESTINATION_METADATA --
 *     Metadata kept along side a file handle to track holes in the destination file.
 */
typedef struct {
    WT_FILE_HANDLE *fh;
    bool complete;

    /* We need to get back to the file system when checking for tombstone files. */
    WT_LIVE_RESTORE_FS *back_pointer;

    /*
     * The hole list tracks which ranges in the destination file are holes. As the migration
     * continues the holes will be gradually filled by either data from the source or new writes.
     * Holes in these extents should only shrink and never grow.
     */
    WT_LIVE_RESTORE_HOLE_NODE *hole_list_head;
} WT_LIVE_RESTORE_DESTINATION_METADATA;

/*
 * __wt_live_restore_file_handle --
 *     A file handle in a live restore file system.
 */
struct __wt_live_restore_file_handle {
    WT_FILE_HANDLE iface;
    WT_FILE_HANDLE *source;
    WT_LIVE_RESTORE_DESTINATION_METADATA destination;

    WT_FS_OPEN_FILE_TYPE file_type;
};

typedef enum {
    WT_LIVE_RESTORE_FS_LAYER_DESTINATION,
    WT_LIVE_RESTORE_FS_LAYER_SOURCE
} WT_LIVE_RESTORE_FS_LAYER_TYPE;

/*
 * __wt_live_restore_fs_layer --
 *     A layer in the live restore file system.
 */
struct __wt_live_restore_fs_layer {
    const char *home;
    WT_LIVE_RESTORE_FS_LAYER_TYPE which;
};

/*
 * __wt_live_restore_fs --
 *     A live restore file system in the user space, which consists of a source and destination
 *     layer.
 */
struct __wt_live_restore_fs {
    WT_FILE_SYSTEM iface;
    WT_FILE_SYSTEM *os_file_system; /* The storage file system. */
    WT_LIVE_RESTORE_FS_LAYER destination;
    WT_LIVE_RESTORE_FS_LAYER source;

    uint8_t background_threads_max;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_LIVE_RESTORE_DEBUG_FILL_HOLES_ON_CLOSE 0x1u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t debug_flags;
};

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

extern int __wti_live_restore_fs_fill_holes(WT_FILE_HANDLE *fh, WT_SESSION *wt_session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));

#ifdef HAVE_UNITTEST

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */
