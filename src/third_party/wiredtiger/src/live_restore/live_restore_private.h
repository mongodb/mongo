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
 * __wt_live_restore_file_handle --
 *     A file handle in a live restore file system.
 */
struct __wt_live_restore_file_handle {
    WT_FILE_HANDLE iface;
    WT_FILE_HANDLE *source;
    /* Metadata kept along side a file handle to track holes in the destination file. */
    struct {
        WT_FILE_HANDLE *fh;
        bool complete;

        /* We need to get back to the file system when checking for tombstone files. */
        WT_LIVE_RESTORE_FS *back_pointer;

        /*
         * The hole list tracks which ranges in the destination file are holes. As the migration
         * continues the holes will be gradually filled by either data from the source or new
         * writes. Holes in these extents should only shrink and never grow.
         */
        WT_LIVE_RESTORE_HOLE_NODE *hole_list_head;
    } destination;

    WT_FS_OPEN_FILE_TYPE file_type;
    WT_RWLOCK ext_lock; /* File extent list lock */
};

/*
 * WT_WITH_LIVE_RESTORE_EXTENT_LIST_WRITE_LOCK --
 *     Acquire the extent list write lock and perform an operation.
 */
#define WT_WITH_LIVE_RESTORE_EXTENT_LIST_WRITE_LOCK(session, lr_fh, op) \
    do {                                                                \
        __wt_writelock((session), &(lr_fh)->ext_lock);                  \
        op;                                                             \
        __wt_writeunlock((session), &(lr_fh)->ext_lock);                \
    } while (0)

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
};

/*
 * WT_LIVE_RESTORE_WORK_ITEM --
 *     A single item of work to be worked on by a thread.
 */
struct __wt_live_restore_work_item {
    char *uri;
    TAILQ_ENTRY(__wt_live_restore_work_item) q; /* List of URIs queued for background migration. */
};

/*
 * WT_LIVE_RESTORE_SERVER --
 *     The live restore server object that is kept on the connection. Holds a thread group and the
 *     work queue, with some additional info.
 */
struct __wt_live_restore_server {
    WT_THREAD_GROUP threads;
    wt_shared uint32_t threads_working;
    WT_SPINLOCK queue_lock;
    uint64_t work_items_remaining;

    TAILQ_HEAD(__wt_live_restore_work_queue, __wt_live_restore_work_item) work_queue;
};

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

extern int __wti_live_restore_fs_fill_holes(WT_FILE_HANDLE *fh, WT_SESSION *wt_session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));

#ifdef HAVE_UNITTEST

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */
