/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

typedef enum {
    WT_THROTTLE_CKPT,  /* Checkpoint throttle */
    WT_THROTTLE_EVICT, /* Eviction throttle */
    WT_THROTTLE_LOG,   /* Logging throttle */
    WT_THROTTLE_READ   /* Read throttle */
} WT_THROTTLE_TYPE;

#define WT_THROTTLE_MIN WT_MEGABYTE /* Config minimum size */

/*
 * The per-file threshold means we won't start the background fsync on a file until it crosses the
 * per-file threshold of data written. The other minimum threshold defines a minimum threshold for
 * the background thread. Otherwise we compute a percentage of the given capacity.
 */
#define WT_CAPACITY_FILE_THRESHOLD (WT_MEGABYTE / 2)
#define WT_CAPACITY_MIN_THRESHOLD (10 * WT_MEGABYTE)
#define WT_CAPACITY_PCT 10

/*
 * If we're being asked to sleep a short amount of time, ignore it. A non-zero value means there may
 * be a temporary violation of the capacity limitation, but one that would even out. That is,
 * possibly fewer sleeps with the risk of more choppy behavior as this number is larger.
 */
#define WT_CAPACITY_SLEEP_CUTOFF_US 100

/*
 * When given a total capacity, divide it up for each subsystem. These defines represent the
 * percentage of the total capacity that we allow for each subsystem capacity. We allow and expect
 * the sum of the subsystems to exceed 100, as often they are not at their maximum at the same time.
 * In any event, we track the total capacity separately, so it is never exceeded.
 */
#define WT_CAPACITY_SYS(total, pct) ((total) * (pct) / 100)
#define WT_CAP_CKPT 5
#define WT_CAP_EVICT 50
#define WT_CAP_LOG 30
#define WT_CAP_READ 55

struct __wt_capacity {
    uint64_t ckpt;      /* Bytes/sec checkpoint capacity */
    uint64_t evict;     /* Bytes/sec eviction capacity */
    uint64_t log;       /* Bytes/sec logging capacity */
    uint64_t read;      /* Bytes/sec read capacity */
    uint64_t total;     /* Bytes/sec total capacity */
    uint64_t threshold; /* Capacity size period */

    volatile uint64_t written; /* Written this period */
    volatile bool signalled;   /* Capacity signalled */

    /*
     * A reservation is a point in time when a read or write for a subsystem can be scheduled, so as
     * not to overrun the given capacity. These values hold the next available reservation, in
     * nanoseconds since the epoch. Getting a reservation with a future time implies sleeping until
     * that time; getting a reservation with a past time implies that the operation can be done
     * immediately.
     */
    uint64_t reservation_ckpt;  /* Atomic: next checkpoint write */
    uint64_t reservation_evict; /* Atomic: next eviction write */
    uint64_t reservation_log;   /* Atomic: next logging write */
    uint64_t reservation_read;  /* Atomic: next read */
    uint64_t reservation_total; /* Atomic: next operation of any kind */
};
