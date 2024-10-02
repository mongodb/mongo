/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * Tuning constants: I hesitate to call this tuning, but we want to review some number of pages from
 * each file's in-memory tree for each page we evict.
 */
#define WT_EVICT_MAX_TREES WT_THOUSAND /* Maximum walk points */
#define WT_EVICT_WALK_BASE 300         /* Pages tracked across file visits */
#define WT_EVICT_WALK_INCR 100         /* Pages added each walk */

/*
 * WT_EVICT_ENTRY --
 *	Encapsulation of an eviction candidate.
 */
struct __wt_evict_entry {
    WT_BTREE *btree; /* Enclosing btree object */
    WT_REF *ref;     /* Page to flush/evict */
    uint64_t score;  /* Relative eviction priority */
};

#define WT_EVICT_QUEUE_MAX 3    /* Two ordinary queues plus urgent */
#define WT_EVICT_URGENT_QUEUE 2 /* Urgent queue index */

/*
 * WT_EVICT_QUEUE --
 *	Encapsulation of an eviction candidate queue.
 */
struct __wt_evict_queue {
    WT_SPINLOCK evict_lock;                /* Eviction LRU queue */
    WT_EVICT_ENTRY *evict_queue;           /* LRU pages being tracked */
    WT_EVICT_ENTRY *evict_current;         /* LRU current page to be evicted */
    uint32_t evict_candidates;             /* LRU list pages to evict */
    uint32_t evict_entries;                /* LRU entries in the queue */
    wt_shared volatile uint32_t evict_max; /* LRU maximum eviction slot used */
};

struct __wt_evict {
    wt_shared volatile uint64_t eviction_progress; /* Eviction progress count */
    uint64_t last_eviction_progress;               /* Tracked eviction progress */

    uint64_t app_waits;  /* User threads waited for eviction */
    uint64_t app_evicts; /* Pages evicted by user threads */

    wt_shared uint64_t evict_max_page_size; /* Largest page seen at eviction */
    wt_shared uint64_t evict_max_ms;        /* Longest milliseconds spent at a single eviction */
    uint64_t reentry_hs_eviction_ms;        /* Total milliseconds spent inside a nested eviction */
    struct timespec stuck_time;             /* Stuck time */

    /*
     * Read information.
     */
    uint64_t read_gen;        /* Current page read generation */
    uint64_t read_gen_oldest; /* Oldest read generation the eviction
                               * server saw in its last queue load */
    uint64_t evict_pass_gen;  /* Number of eviction passes */

    /*
     * Eviction thread information.
     */
    WT_CONDVAR *evict_cond;      /* Eviction server condition */
    WT_SPINLOCK evict_walk_lock; /* Eviction walk location */

    /*
     * Eviction threshold percentages use double type to allow for specifying percentages less than
     * one.
     */
    wt_shared double eviction_dirty_target;  /* Percent to allow dirty */
    wt_shared double eviction_dirty_trigger; /* Percent to trigger dirty eviction */
    double eviction_trigger;                 /* Percent to trigger eviction */
    double eviction_target;                  /* Percent to end eviction */
    double eviction_updates_target;          /* Percent to allow for updates */
    double eviction_updates_trigger;         /* Percent of updates to trigger eviction */

    double eviction_checkpoint_target; /* Percent to reduce dirty to during checkpoint scrubs */
    wt_shared double eviction_scrub_target; /* Current scrub target */

    uint64_t cache_max_wait_us;      /* Maximum time an operation waits for space in cache */
    uint64_t cache_stuck_timeout_ms; /* Maximum time the cache can be stuck for in diagnostic mode
                                        before timing out */

    /*
     * Eviction thread tuning information.
     */
    uint32_t evict_tune_datapts_needed;          /* Data needed to tune */
    struct timespec evict_tune_last_action_time; /* Time of last action */
    struct timespec evict_tune_last_time;        /* Time of last check */
    uint32_t evict_tune_num_points;              /* Number of values tried */
    uint64_t evict_tune_progress_last;           /* Progress counter */
    uint64_t evict_tune_progress_rate_max;       /* Max progress rate */
    bool evict_tune_stable;                      /* Are we stable? */
    uint32_t evict_tune_workers_best;            /* Best performing value */

    /*
     * Pass interrupt counter.
     */
    wt_shared volatile uint32_t pass_intr; /* Interrupt eviction pass. */

    /*
     * LRU eviction list information.
     */
    WT_SPINLOCK evict_pass_lock;   /* Eviction pass lock */
    WT_SESSION_IMPL *walk_session; /* Eviction pass session */
    WT_DATA_HANDLE *walk_tree;     /* LRU walk current tree */

    WT_SPINLOCK evict_queue_lock; /* Eviction current queue lock */
    WT_EVICT_QUEUE evict_queues[WT_EVICT_QUEUE_MAX];
    WT_EVICT_QUEUE *evict_current_queue; /* LRU current queue in use */
    WT_EVICT_QUEUE *evict_fill_queue;    /* LRU next queue to fill.
                                            This is usually the same as the
                                            "other" queue but under heavy
                                            load the eviction server will
                                            start filling the current queue
                                            before it switches. */
    WT_EVICT_QUEUE *evict_other_queue;   /* LRU queue not in use */
    WT_EVICT_QUEUE *evict_urgent_queue;  /* LRU urgent queue */
    uint32_t evict_slots;                /* LRU list eviction slots */

#define WT_EVICT_PRESSURE_THRESHOLD 0.95
#define WT_EVICT_SCORE_BUMP 10
#define WT_EVICT_SCORE_CUTOFF 10
#define WT_EVICT_SCORE_MAX 100
    /*
     * Score of how aggressive eviction should be about selecting eviction candidates. If eviction
     * is struggling to make progress, this score rises (up to a maximum of WT_EVICT_SCORE_MAX), at
     * which point the cache is "stuck" and transactions will be rolled back.
     */
    wt_shared uint32_t evict_aggressive_score;

    /*
     * Score of how often LRU queues are empty on refill. This score varies between 0 (if the queue
     * hasn't been empty for a long time) and 100 (if the queue has been empty the last 10 times we
     * filled up.
     */
    uint32_t evict_empty_score;

/*
 * Flags.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_EVICT_CACHE_CLEAN 0x001u        /* Evict clean pages */
#define WT_EVICT_CACHE_CLEAN_HARD 0x002u   /* Clean % blocking app threads */
#define WT_EVICT_CACHE_DIRTY 0x004u        /* Evict dirty pages */
#define WT_EVICT_CACHE_DIRTY_HARD 0x008u   /* Dirty % blocking app threads */
#define WT_EVICT_CACHE_NOKEEP 0x010u       /* Don't add read pages to cache */
#define WT_EVICT_CACHE_SCRUB 0x020u        /* Scrub dirty pages */
#define WT_EVICT_CACHE_UPDATES 0x040u      /* Evict pages with updates */
#define WT_EVICT_CACHE_UPDATES_HARD 0x080u /* Update % blocking app threads */
#define WT_EVICT_CACHE_URGENT 0x100u       /* Pages are in the urgent queue */
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
#define WT_EVICT_CACHE_ALL (WT_EVICT_CACHE_CLEAN | WT_EVICT_CACHE_DIRTY | WT_EVICT_CACHE_UPDATES)
#define WT_EVICT_CACHE_HARD \
    (WT_EVICT_CACHE_CLEAN_HARD | WT_EVICT_CACHE_DIRTY_HARD | WT_EVICT_CACHE_UPDATES_HARD)
    uint32_t flags;
};

#define WT_WITH_PASS_LOCK(session, op)                                                   \
    do {                                                                                 \
        WT_WITH_LOCK_WAIT(session, &evict->evict_pass_lock, WT_SESSION_LOCKED_PASS, op); \
    } while (0)

/* Flags used with __wt_evict */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_EVICT_CALL_CLOSING 0x1u  /* Closing connection or tree */
#define WT_EVICT_CALL_NO_SPLIT 0x2u /* Splits not allowed */
#define WT_EVICT_CALL_URGENT 0x4u   /* Urgent eviction */
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
