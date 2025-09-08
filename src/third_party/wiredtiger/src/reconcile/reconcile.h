/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 * All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_REC_APP_EVICTION_SNAPSHOT 0x0001u
#define WT_REC_CALL_URGENT 0x0002u
#define WT_REC_CHECKPOINT 0x0004u
#define WT_REC_CHECKPOINT_RUNNING 0x0008u
#define WT_REC_CLEAN_AFTER_REC 0x0010u
#define WT_REC_EVICT 0x0020u
#define WT_REC_EVICT_CALL_CLOSING 0x0040u
#define WT_REC_HS 0x0080u
#define WT_REC_IN_MEMORY 0x0100u
#define WT_REC_REWRITE_DELTA 0x0200u
#define WT_REC_SCRUB 0x0400u
#define WT_REC_VISIBILITY_ERR 0x0800u
#define WT_REC_VISIBLE_NO_SNAPSHOT 0x1000u
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

extern bool __wt_rec_in_progress(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bulk_init(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bulk_insert_fix(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk, bool deleted)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bulk_insert_fix_bitmap(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bulk_insert_row(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bulk_insert_var(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk, bool deleted)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_bulk_wrapup(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_ovfl_discard_add(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL *cell)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __wt_reconcile(WT_SESSION_IMPL *session, WT_REF *ref, WT_SALVAGE_COOKIE *salvage,
  uint32_t flags) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern uint32_t __wt_split_page_size(int split_pct, uint32_t maxpagesize, uint32_t allocsize)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void __wt_ovfl_discard_free(WT_SESSION_IMPL *session, WT_PAGE *page);
extern void __wt_ovfl_reuse_free(WT_SESSION_IMPL *session, WT_PAGE *page);

#ifdef HAVE_UNITTEST
extern int __ut_ovfl_discard_verbose(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL *cell,
  const char *tag) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_ovfl_discard_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern int __ut_ovfl_track_init(WT_SESSION_IMPL *session, WT_PAGE *page)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */
