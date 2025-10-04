/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 * All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * Tuning constants: I hesitate to call this tuning, but we want to review some number of pages from
 * each file's in-memory tree for each page we evict.
 */
#define WTI_EVICT_MAX_TREES WT_THOUSAND /* Maximum walk points */
#define WTI_EVICT_WALK_BASE 300         /* Pages tracked across file visits */
#define WTI_EVICT_WALK_INCR 100         /* Pages added each walk */

/*
 * WTI_EVICT_ENTRY --
 *	Encapsulation of an eviction candidate.
 */
struct __wti_evict_entry {
    WT_BTREE *btree; /* Enclosing btree object */
    WT_REF *ref;     /* Page to flush/evict */
    uint64_t score;  /* Relative eviction priority */
};

#define WTI_EVICT_QUEUE_MAX 3    /* Two ordinary queues plus urgent */
#define WTI_EVICT_URGENT_QUEUE 2 /* Urgent queue index */

/*
 * WTI_EVICT_QUEUE --
 *	Encapsulation of an eviction candidate queue.
 */
struct __wti_evict_queue {
    WT_SPINLOCK evict_lock;                /* Eviction LRU queue */
    WTI_EVICT_ENTRY *evict_queue;          /* LRU pages being tracked */
    WTI_EVICT_ENTRY *evict_current;        /* LRU current page to be evicted */
    uint32_t evict_candidates;             /* LRU list pages to evict */
    uint32_t evict_entries;                /* LRU entries in the queue */
    wt_shared volatile uint32_t evict_max; /* LRU maximum eviction slot used */
};

#define WTI_WITH_PASS_LOCK(session, op)                                                  \
    do {                                                                                 \
        WT_WITH_LOCK_WAIT(session, &evict->evict_pass_lock, WT_SESSION_LOCKED_PASS, op); \
    } while (0)

/* DO NOT EDIT: automatically built by prototypes.py: BEGIN */

extern int __wti_evict_app_assist_worker(WT_SESSION_IMPL *session, bool busy, bool readonly,
  bool interruptible) WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
extern void __wti_evict_list_clear_page(WT_SESSION_IMPL *session, WT_REF *ref);
static WT_INLINE bool __wti_evict_hs_dirty(WT_SESSION_IMPL *session)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wti_evict_readgen_is_soon_or_wont_need(uint64_t *readgen)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE bool __wti_evict_updates_needed(WT_SESSION_IMPL *session, double *pct_fullp)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE double __wti_evict_dirty_target(WT_EVICT *evict)
  WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result));
static WT_INLINE void __wti_evict_read_gen_bump(WT_SESSION_IMPL *session, WT_PAGE *page);
static WT_INLINE void __wti_evict_read_gen_new(WT_SESSION_IMPL *session, WT_PAGE *page);

#ifdef HAVE_UNITTEST

#endif

/* DO NOT EDIT: automatically built by prototypes.py: END */
