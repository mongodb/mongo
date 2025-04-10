/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * Stop files are created in the file system to indicate that the source directory should never be
 * used for the filename indicated.
 *
 * For example "foo.wt" would have a stop file "foo.wt.stop", this could mean a number of things:
 *  - The file foo.wt may have completed migration.
 *  - It may have been removed, in this case we create a stop file in case the same name "foo.wt" is
 *    recreated.
 *  - It may have been renamed, again we create a stop file in case it is recreated.
 */
#define WTI_LIVE_RESTORE_STOP_FILE_SUFFIX ".stop"
#define WTI_LIVE_RESTORE_TEMP_FILE_SUFFIX ".lr_tmp"
/*
 * WTI_OFFSET_END returns the last byte used by a range (exclusive). i.e. if we have an offset=0 and
 * length=1024 WTI_OFFSET_END returns 1024
 */
#define WTI_OFFSET_END(offset, len) (offset + (wt_off_t)len)
#define WTI_OFFSET_TO_BIT(offset) (uint64_t)((offset) / (wt_off_t)lr_fh->allocsize)
#define WTI_BIT_TO_OFFSET(bit) (wt_off_t)((bit)*lr_fh->allocsize)
/*
 * The end of the bitmap is the portion of the file represented by the bitmap. i.e. a file with
 * 2*4096 blocks will have a size of 8192 bytes represented.
 */
#define WTI_BITMAP_END(lr_fh) ((wt_off_t)(lr_fh)->allocsize * (wt_off_t)(lr_fh)->nbits)

/*
 * We close the backing source file when migration completes. If we've closed it, or the source file
 * doesn't exist, there is no more migration work to do.
 */
#define WTI_DEST_COMPLETE(lr_fh) ((lr_fh)->source == NULL)
/*
 * The most aggressive sweep server configuration runs every second. Allow 4 seconds to make sure
 * the server has time to find and close any open file handles.
 */
#define WT_LIVE_RESTORE_TIMING_STRESS_CLEAN_UP_DELAY 4

/*
 * __wti_live_restore_file_handle --
 *     A file handle in a live restore file system.
 */
struct __wti_live_restore_file_handle {
    WT_FILE_HANDLE iface;
    WT_FILE_HANDLE *destination;
    /* We need to get back to the file system when checking state. */
    WTI_LIVE_RESTORE_FS *back_pointer;
    uint32_t allocsize;
    WT_FS_OPEN_FILE_TYPE file_type;

    /*
     * This lock protects all of the below fields and should be held for all accesses to them. There
     * are a few rare exceptions which are listed when they occur.
     */
    WT_RWLOCK lock;
    /* Number of bits in the bitmap, should be equivalent to source file size / alloc_size. */
    uint64_t nbits;
    /*
     * The live restore bitmap size is set once on bitmap creation and will never change, which
     * means it is always the size of the source file. This means reads or writes beyond the end of
     * the bitmap are only relevant to the destination file. The destination file could shrink to be
     * smaller than the original source file size, in this case we could shrink the bitmap but we
     * have chosen not to as that is an optimization.
     */
    uint8_t *bitmap;
    WT_FILE_HANDLE *source;
};

/*
 * WTI_WITH_LIVE_RESTORE_FH_WRITE_LOCK --
 *     Acquire the bitmap list write lock and perform an operation.
 */
#define WTI_WITH_LIVE_RESTORE_FH_WRITE_LOCK(session, lr_fh, op) \
    do {                                                        \
        __wt_writelock((session), &(lr_fh)->lock);              \
        op;                                                     \
        __wt_writeunlock((session), &(lr_fh)->lock);            \
    } while (0)

/*
 * WTI_WITH_LIVE_RESTORE_QUEUE_LOCK --
 *     Acquire the background migration queue lock and perform an operation.
 */
#define WTI_WITH_LIVE_RESTORE_QUEUE_LOCK(session, op)                                \
    do {                                                                             \
        __wt_spin_lock((session), &S2C(session)->live_restore_server->queue_lock);   \
        op;                                                                          \
        __wt_spin_unlock((session), &S2C(session)->live_restore_server->queue_lock); \
    } while (0)

/*
 * WTI_WITH_LIVE_RESTORE_STATE_LOCK --
 *	Acquire the state lock, perform an operation, drop the lock.
 */
#define WTI_WITH_LIVE_RESTORE_STATE_LOCK(session, lr_fs, op) \
    WT_WITH_LOCK_WAIT((session), &(lr_fs)->state_lock, WT_SESSION_LOCKED_LIVE_RESTORE_STATE, op)

typedef enum {
    WTI_LIVE_RESTORE_FS_LAYER_DESTINATION,
    WTI_LIVE_RESTORE_FS_LAYER_SOURCE,
    WTI_LIVE_RESTORE_FS_LAYER_NONE,
} WTI_LIVE_RESTORE_FS_LAYER_TYPE;

/*
 * __wti_live_restore_fs_layer --
 *     A layer in the live restore file system.
 */
struct __wti_live_restore_fs_layer {
    const char *home;
    WTI_LIVE_RESTORE_FS_LAYER_TYPE which;
};

/*
 * Live restore states. As live restore progresses we will transition through each of these states
 * one by one. Live restore transitions through each state in the order they are listed below.
 */
typedef enum {
    /*
     * This is not a valid state. We return it when there is no state file on disk and therefore
     * we're not in live restore yet.
     */
    WTI_LIVE_RESTORE_STATE_NONE,
    /*
     * The background migration state is where the majority is where the majority of work takes
     * place. Users can perform reads/writes while we copy backing data to the destination in the
     * background.
     */
    WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION,
    /* We've completed background migration and are now cleaning up any live restore metadata. */
    WTI_LIVE_RESTORE_STATE_CLEAN_UP,
    /* We've completed the live restore. */
    WTI_LIVE_RESTORE_STATE_COMPLETE
} WTI_LIVE_RESTORE_STATE;

/*
 * __wti_live_restore_fs --
 *     A live restore file system in the user space, which consists of a source and destination
 *     layer.
 */
struct __wti_live_restore_fs {
    WT_FILE_SYSTEM iface;
    WT_FILE_SYSTEM *os_file_system; /* The storage file system. */
    WTI_LIVE_RESTORE_FS_LAYER destination;
    WTI_LIVE_RESTORE_FS_LAYER source;

    uint8_t background_threads_max;
    size_t read_size;

    WTI_LIVE_RESTORE_STATE state;
    WT_SPINLOCK state_lock;
};

/*
 * WTI_LIVE_RESTORE_WORK_ITEM --
 *     A single item of work to be worked on by a thread.
 */
struct __wti_live_restore_work_item {
    char *uri;
    TAILQ_ENTRY(__wti_live_restore_work_item) q; /* List of URIs queued for background migration. */
};

/*
 * WTI_LIVE_RESTORE_SERVER --
 *     The live restore server object that is kept on the connection. Holds a thread group and the
 *     work queue, with some additional info.
 */
struct __wti_live_restore_server {
    WT_THREAD_GROUP threads;
    wt_shared uint32_t threads_working;
    WT_SPINLOCK queue_lock;
    WT_TIMER msg_timer;
    WT_TIMER start_timer;
    uint64_t msg_count;
    uint64_t work_count;
    uint64_t work_items_remaining;
    bool shutting_down; /* Set when connection close terminates the server. */

    TAILQ_HEAD(__wti_live_restore_work_queue, __wti_live_restore_work_item) work_queue;
};

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

extern WTI_LIVE_RESTORE_STATE __wti_live_restore_get_state(WT_SESSION_IMPL *session,
  WTI_LIVE_RESTORE_FS *lr_fs) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern bool __wti_live_restore_migration_complete(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_live_restore_cleanup_stop_files(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_live_restore_fs_restore_file(WT_FILE_HANDLE *fh, WT_SESSION *wt_session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_live_restore_init_state(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_live_restore_set_state(WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FS *lr_fs,
  WTI_LIVE_RESTORE_STATE new_state) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wti_live_restore_validate_directories(WT_SESSION_IMPL *session,
  WTI_LIVE_RESTORE_FS *lr_fs) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));

#ifdef HAVE_UNITTEST

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */
