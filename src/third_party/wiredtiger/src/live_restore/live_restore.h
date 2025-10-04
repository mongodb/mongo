/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#define WT_LIVE_RESTORE_STATE_STRING_MAX 128

/*
 * __wt_live_restore_fh_meta --
 *     File handle metadata persisted to the WiredTiger metadata file.
 */
struct __wt_live_restore_fh_meta {
    char *bitmap_str;
    /*
     * The number of bits in the bitmap. We use -1 as a special case to identify when a file has
     * finished migration and no longer needs a bitmap.
     */
    int64_t nbits;
    uint32_t allocsize;
};

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

extern bool __wt_live_restore_migration_in_progress(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_live_restore_clean_metadata_string(WT_SESSION_IMPL *session, char *value)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_live_restore_fh_to_metadata(WT_SESSION_IMPL *session, WT_FILE_HANDLE *fh,
  WT_ITEM *meta_string) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_live_restore_get_state_string(WT_SESSION_IMPL *session, WT_ITEM *lr_state_str)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_live_restore_metadata_to_fh(WT_SESSION_IMPL *session, WT_FILE_HANDLE *fh,
  WT_LIVE_RESTORE_FH_META *lr_fh_meta) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_live_restore_server_create(WT_SESSION_IMPL *session, const char *cfg[])
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_live_restore_server_destroy(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_live_restore_turtle_read(WT_SESSION_IMPL *session, const char *key, char **valuep)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_live_restore_turtle_rewrite(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_live_restore_turtle_update(WT_SESSION_IMPL *session, const char *key,
  const char *value, bool take_turtle_lock) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_live_restore_validate_non_lr_system(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_os_live_restore_fs(WT_SESSION_IMPL *session, const char *cfg[],
  const char *destination, WT_FILE_SYSTEM **fsp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void __wt_live_restore_init_stats(WT_SESSION_IMPL *session);

#ifdef HAVE_UNITTEST
extern int __ut_live_restore_compute_read_end_bit(WT_SESSION_IMPL *session,
  WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, wt_off_t buf_size, uint64_t first_clear_bit,
  uint64_t *end_bitp) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_live_restore_decode_bitmap(WT_SESSION_IMPL *session, const char *bitmap_str,
  uint64_t nbits, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_live_restore_encode_bitmap(
  WT_SESSION_IMPL *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_ITEM *buf)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_live_restore_fill_hole(WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_SESSION *wt_session,
  char *buf, wt_off_t buf_size, wt_off_t *read_offsetp, bool *finishedp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void __ut_live_restore_fh_fill_bit_range(
  WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, WT_SESSION_IMPL *session, wt_off_t offset, size_t len);

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */
