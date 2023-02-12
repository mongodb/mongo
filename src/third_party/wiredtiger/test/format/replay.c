/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "format.h"

/*
 * Predictable replay is the ability to do test runs multiple times and always have predictable
 * changes made at every timestamp. Two predictable runs with the same starting data seed executed
 * up to the same timestamp will always have their data compare identically. Predictable replay only
 * works with timestamped transactions and to avoid complexity, only a single operation is allowed
 * in a transaction.
 *
 * To achieve the predictability we use two random number generators (the data RNG and the extra
 * RNG) with known start seeds, the data seed and the extra seed. Every single-threaded modification
 * (like bulk loading) when deciding on a random course, uses the global data RNG, which is seeded
 * by the data seed. Global decisions that don't affect data, like whether to turn on verbose, or
 * even the rate of checkpointing, use the global extra RNG, which is seeded by the extra seed.
 * Changing the extra seed may change some characteristics of how a workload is tested, but should
 * not change any data on disk. When worker threads run, they have their own data and extra RNGs,
 * and these are seeded by the timestamp they are working on.
 *
 * Before a worker thread can decide on what operation to do on which key in which table, it must
 * obtain the next timestamp. Timestamps are doled out atomically, so no two worker threads can ever
 * perform operations using the same timestamp. The timestamp is XOR-ed with the data seed, the
 * result is the seed of the thread's private data RNG for the duration of that operation. Likewise,
 * a private extra RNG is seeded from the timestamp and the extra seed. This ensures that all
 * decisions about what is committed at that timestamp are predictable based on the timestamp. As
 * you might expect, the thread's data RNG is used to decide what operation to do, which table to
 * use, and which key within the table. Other random decisions, like whether to reopen a session, or
 * whether to repeat a read from the snap list, use the extra RNG.
 *
 * Note that once a thread has started to work on an operation at a timestamp, it cannot give up on
 * the effort. If, for example, a rollback error naturally happens, we can rollback the transaction.
 * However, immediately getting a new timestamp would mean that we would lose the consequences of
 * the previous timestamp, perhaps a record would not be updated in a particular way. Thus, after a
 * rollback, a thread starts again, using the same timestamp it had before, and it seeds its RNGs
 * again using this timestamp. This gives full predictability, even in the face of temporary
 * failures.
 *
 * To avoid the possibility that two threads work on the same key at the same time, we have the
 * concept of lanes, and only one thread can be working in a lane at once. There are LANE_COUNT
 * lanes, where LANE_COUNT is 2^k for some k. A thread uses a data RNG to choose the top bits of a
 * key number, but the bottom k bits of the key number are set to the bottom k bits of the timestamp
 * being worked. Those bottom k bits also determine the lane we are in. Each lane has a flag that
 * determines whether the lane is in use by some operation. If thread T1 working an operation at
 * timestamp X takes a sufficiently long time relative to other operations, it may be that the
 * current timestamp has advanced to X + LANE_COUNT. If that is the case, a different thread T2 that
 * gets that larger timestamp will see that the lane is occupied. Rather than using that timestamp
 * and potentially getting the same key number, the T2 leaves that timestamp, knowing that T1 will
 * do it, and advances to another timestamp to work on. When T1 finishes its long operation, it will
 * notice if there are other timestamps that have been left for it. If so, it keeps the lane
 * occupied, and works on the new timestamp. At some point, it will notice that all the timestamps
 * in the lane have been processed up to that point, and it can release the lane, and go back to
 * choosing the next available timestamp to process.
 *
 * Having some operations lag behind is a natural part of processing. This leads to a stable
 * timestamp that may lag significantly. Due to the possibility of dependencies between operations,
 * the more lag, the more chance that a rollback error occurs. Without predictable replay, this is
 * not a problem, any operation that produces a rollback can be freely abandoned, and threads
 * generally continue moving quickly ahead with more work. However, with predictable replay, no
 * operation can be abandoned, and an operation that failed because of a dependency will repeatedly
 * fail until the stable timestamp advances. For that reason, we keep calculating and moving the
 * stable timestamp ahead at a much faster pace when predictable replay is configured. We also use
 * an algorithm that only uses lanes that are in use to calculate the stable timestamp. This is safe
 * and more responsive than the default calculation. And when there is a rollback error, we try to
 * be smart whether we need to yield or pause. These modifications allow predictable performance to
 * be on par with regular performance.
 */

/*
 * replay_end_timed_run --
 *     In a timed run, get everyone to stop.
 */
void
replay_end_timed_run(void)
{
    /*
     * We'll post a stop timestamp that all worker threads should abide by. There's a potential race
     * between when we read the current timestamp and before we publish the stop timestamp. During
     * that time, other threads could do work and advance the current timestamp, potentially beyond
     * the intended stop timestamp. We pick a stop timestamp far enough in the future that it's
     * rather unlikely to happen.
     */
    WT_PUBLISH(g.stop_timestamp, g.timestamp + 0x10000);
}

/*
 * replay_maximum_committed --
 *     For predictable replay runs, return the largest timestamp that's no longer in use.
 */
uint64_t
replay_maximum_committed(void)
{
    uint64_t commit_ts, ts;
    uint32_t lane;

    /*
     * The calculation is expensive, and does not need to be accurate all the time, and it's okay to
     * be behind. So we use a cached value most of the time.
     */
    ts = g.replay_cached_committed;
    if (ts == 0 || __wt_atomic_addv32(&g.replay_calculate_committed, 1) % 20 == 0) {
        WT_ORDERED_READ(ts, g.timestamp);
        testutil_check(pthread_rwlock_wrlock(&g.lane_lock));
        for (lane = 0; lane < LANE_COUNT; ++lane) {
            if (g.lanes[lane].in_use) {
                commit_ts = g.lanes[lane].last_commit_ts;
                if (commit_ts != 0)
                    ts = WT_MIN(ts, commit_ts);
            }
        }
        if (ts == 0)
            ts = 1;
        g.replay_cached_committed = ts;
        testutil_check(pthread_rwlock_unlock(&g.lane_lock));
    }
    return (ts);
}

/*
 * replay_operation_enabled --
 *     Return whether an operation type should be enabled in the configuration.
 */
bool
replay_operation_enabled(thread_op op)
{
    if (!GV(RUNS_PREDICTABLE_REPLAY))
        return (true);

    /*
     * We don't permit modify operations with predictable replay.
     *
     * The problem is read timestamps. As currently implemented, the read timestamp selected is
     * variable, based on the state of other threads and their progress with other timestamped
     * operations. And if two changes are made to the same key in a short amount of time, if the
     * second operation were to be performed sometimes with a read timestamp before the first
     * operation, and sometimes with a read timestamp after the first operation, then the results
     * would be variable.
     *
     * We could track recent operations on a key (in its lane, for instance), but when we realize
     * the read timestamp isn't recent enough, we would need to wait for the stable timestamp to
     * move forward (and our waiting can affect/delay other thread's operations as well). Having the
     * stable timestamp move forward is the only way our read timestamp can progress.
     *
     * Another possibility that also involves tracking recent operations on a key would be to
     * disallow modifies that occur within, say 10000 timestamps of a previous write operation on
     * the same key. Those modifies could be silently converted to reads, for instance. If our read
     * timestamp was greater than 10000 timestamps behind, we'd still need to wait for the stable
     * timestamp to catch up.
     */
    if (op == MODIFY)
        return (false);

    /*
     * FIXME-WT-10570. We don't permit remove operations with predictable replay.
     *
     * This should be something we can and should fix. The problem may be similar to the problem
     * with modify, where having a varying read timestamp can cause different results for different
     * runs.
     */
    if (op == REMOVE)
        return (false);

    /*
     * We don't permit truncate operations with predictable replay.
     *
     * Currently, we use an operation's timestamp to help derive the operation's key.
     * The last N bits of the timestamp are used as the last bits of the key (where
     * 2^N == LANE_COUNT). These last N bits give the lane number, and within each
     * lane we track the progress of operations for that lane. Using lanes, we can
     * track and guarantee that only a single operation is active in a lane at once,
     * and therefore we can't have multiple operations on a single key performed out
     * of order or simultaneously. The truncate operation, for a small set of keys,
     * would reserve multiple consecutive lanes (probably okay) and for larger sets,
     * would reserve the entire set of lanes. This would effectively require all
     * threads to get into a holding state, waiting for the truncate to start and then
     * complete before continuing with their next operation. While we could fudge this
     * in certain ways (e.g. operations with 10000 timestamps of a truncate would be
     * forced to stay out of its table), there still would be a lot of details, and
     * some rethink of our lane strategy. Even getting this to work, we would have
     * a truncate that had the whole table to itself, which doesn't seem like an
     * effective test.
     */
    if (op == TRUNCATE)
        return (false);

    return (true);
}

/*
 * replay_pick_timestamp --
 *     Pick the next timestamp for this operation. That timestamp is used for any commits and also
 *     determines which lane we are in, to prevent races from occurring on operations on a single
 *     key. Also, by using the timestamp to seed the random number generators, it also determines
 *     precisely the nature of the operation.
 */
static void
replay_pick_timestamp(TINFO *tinfo)
{
    uint64_t replay_seed, stop_ts, ts;
    uint32_t lane;
    bool in_use;

    /*
     * Choose a unique timestamp for commits. When we do predictable replay. If the field for
     * replaying again is set, we already have a timestamp picked for us.
     */
    if (tinfo->replay_again) {
        /*
         * Timestamp is already picked for us.
         */
        testutil_assert(tinfo->lane == LANE_NUMBER(tinfo->replay_ts));
        tinfo->replay_again = false;
    } else {
        testutil_assert(tinfo->lane == LANE_NONE);

        stop_ts = g.stop_timestamp;
        if (stop_ts != 0 && g.stable_timestamp >= stop_ts && tinfo->replay_ts == 0) {
            tinfo->quit = true;
            return;
        }

        testutil_check(pthread_rwlock_wrlock(&g.lane_lock));
        do {
            /*
             * For predictable replay, this is the only place we increment the timestamp. We keep a
             * copy to check that assumption. If we were to mistakenly change the timestamp
             * elsewhere (as might be done in non-predictable runs), we would lose the integrity of
             * the predictable run.
             */
            testutil_assert(g.timestamp_copy == g.timestamp);
            ts = __wt_atomic_addv64(&g.timestamp, 1);
            g.timestamp_copy = g.timestamp;
            lane = LANE_NUMBER(ts);
            WT_ORDERED_READ(in_use, g.lanes[lane].in_use);
        } while (in_use);

        tinfo->replay_ts = ts;
        WT_PUBLISH(g.lanes[lane].in_use, true);
        testutil_check(pthread_rwlock_unlock(&g.lane_lock));
        tinfo->lane = lane;
    }

    testutil_assert(tinfo->lane != LANE_NONE);
    testutil_assert(g.lanes[tinfo->lane].in_use);

    /*
     * For this operation, seed the RNG used for data operations according to the timestamp and the
     * global data seed. This allows us to have a predictable set of actions related to commits at
     * this timestamp, so long as we are running with the same global data seed.
     */
    replay_seed = tinfo->replay_ts ^ GV(RANDOM_DATA_SEED);
    testutil_random_from_seed(&tinfo->data_rnd, replay_seed);
    replay_seed = tinfo->replay_ts ^ GV(RANDOM_EXTRA_SEED);
    testutil_random_from_seed(&tinfo->extra_rnd, replay_seed);
}

/*
 * replay_loop_begin --
 *     Called at the top of the operation loop.
 */
void
replay_loop_begin(TINFO *tinfo, bool intxn)
{
    if (GV(RUNS_PREDICTABLE_REPLAY)) {
        /*
         * Predictable replay, as it works now, requires that we're not in transaction when we start
         * the loop.
         */
        testutil_assert(!intxn);

        /*
         * We're here at the start of the loop for one of four reasons:
         *   1) We needed to rollback the transaction, so we didn't give up our replay timestamp,
         * and we set the again flag.
         *   2) We successfully committed the last transaction, but our lane was behind,
         * and was skipped over, so we're obligated to perform the next timestamp in our lane.
         * In that case, we have a replay timestamp and the again flag is set.
         *   3) We successfully committed the last transaction, and our lane was not behind.
         * We don't have a replay timestamp and the again flag is off.
         *   4) It's our first time through the loop, this is equivalent to the previous case.
         */
        testutil_assert(tinfo->replay_again == (tinfo->replay_ts != 0));
        /*
         * Choose a unique timestamp for commits, based on the conditions above.
         */
        replay_pick_timestamp(tinfo);

        testutil_assert(tinfo->quit || tinfo->replay_ts != 0);
    }
}

/*
 * replay_run_reset --
 *     Called at beginning and end of runs to set up the lanes.
 */
static void
replay_run_reset(void)
{
    TINFO *tinfo, **tlp;
    uint64_t ts;
    uint32_t lane;

    /* Set every lane's commit timestamp to the current timestamp. */
    ts = g.timestamp;
    g.timestamp_copy = ts;
    for (lane = 0; lane < LANE_COUNT; ++lane)
        g.lanes[lane].last_commit_ts = ts;
    g.replay_cached_committed = ts;

    /* Reset fields in tinfo. */
    if (tinfo_list != NULL)
        for (tlp = tinfo_list; *tlp != NULL; ++tlp) {
            tinfo = *tlp;
            tinfo->replay_again = false;
            tinfo->replay_ts = 0;
            tinfo->lane = 0;
            tinfo->op = (thread_op)0;
        }
}

/*
 * replay_run_begin --
 *     Called at the beginning of a run.
 */
void
replay_run_begin(WT_SESSION *session)
{
    (void)session;

    if (GV(RUNS_PREDICTABLE_REPLAY))
        replay_run_reset();
}

/*
 * replay_run_end --
 *     Called when finishing processing for a run.
 */
void
replay_run_end(WT_SESSION *session)
{
    (void)session;

    if (GV(RUNS_PREDICTABLE_REPLAY))
        replay_run_reset();
}

/*
 * replay_read_ts --
 *     Return a read timestamp for a begin transaction call.
 */
uint64_t
replay_read_ts(TINFO *tinfo)
{
    uint64_t commit_ts;

    testutil_assert(GV(RUNS_PREDICTABLE_REPLAY) && tinfo->lane != LANE_NONE &&
      g.lanes[tinfo->lane].in_use && tinfo->replay_ts != 0);

    commit_ts = replay_maximum_committed();
    testutil_assert(commit_ts != 0);
    return (commit_ts);
}

/*
 * replay_prepare_ts --
 *     Return a timestamp to be used for prepare.
 */
uint64_t
replay_prepare_ts(TINFO *tinfo)
{
    uint64_t prepare_ts, ts;

    testutil_assert(GV(RUNS_PREDICTABLE_REPLAY));

    /* See if we're just starting a run. */
    if (tinfo->replay_ts == 0 || tinfo->replay_ts <= g.replay_start_timestamp + LANE_COUNT)
        /*
         * When we're starting a run, we'll just use the final commit timestamp for our prepare
         * timestamp. We know that's safe.
         */
        prepare_ts = tinfo->replay_ts;
    else {
        /*
         * Our lane's current operation will have a commit timestamp tinfo->replay_ts. Our lane's
         * previous commit timestamp was that number minus LANE_COUNT. The global stable timestamp
         * generally should not be advanced past our lane's previous commit timestamp. So a prepare
         * timestamp halfway between the lane's previous commit timestamp and the current commit
         * timestamp should be valid.
         */
        ts = tinfo->replay_ts - LANE_COUNT / 2;

        /* As a sanity check, make sure the timestamp hasn't completely aged out. */
        if (ts < g.oldest_timestamp)
            prepare_ts = ts;
        else
            prepare_ts = tinfo->replay_ts;
    }
    return (prepare_ts);
}

/*
 * replay_commit_ts --
 *     Return the commit timestamp.
 */
uint64_t
replay_commit_ts(TINFO *tinfo)
{
    testutil_assert(GV(RUNS_PREDICTABLE_REPLAY));

    testutil_assert(tinfo->replay_ts != 0);
    return (tinfo->replay_ts);
}

/*
 * replay_committed --
 *     Called when a transaction was successfully committed. We can give up a lane if appropriate.
 */
void
replay_committed(TINFO *tinfo)
{
    uint32_t lane;

    if (!GV(RUNS_PREDICTABLE_REPLAY))
        return;

    testutil_assert(tinfo->replay_ts != 0);

    lane = tinfo->lane;
    testutil_assert(!tinfo->replay_again);
    testutil_check(pthread_rwlock_wrlock(&g.lane_lock));

    /*
     * Updating the last commit timestamp for a lane in use allows read, oldest and stable
     * timestamps to advance.
     */
    WT_PUBLISH(g.lanes[lane].last_commit_ts, tinfo->replay_ts);
    if (g.timestamp <= tinfo->replay_ts + LANE_COUNT) {
        WT_PUBLISH(g.lanes[lane].in_use, false);
        tinfo->lane = LANE_NONE;
        tinfo->replay_ts = 0;
    } else {
        tinfo->replay_ts += LANE_COUNT;
        tinfo->replay_again = true;
    }
    testutil_check(pthread_rwlock_unlock(&g.lane_lock));
}

/*
 * replay_adjust_key --
 *     Given a fully random key number, modify the key that is in our lane.
 */
void
replay_adjust_key(TINFO *tinfo, uint64_t max_rows)
{
    uint64_t keyno;
    uint32_t lane;

    if (GV(RUNS_PREDICTABLE_REPLAY)) {
        lane = tinfo->lane;
        keyno = (tinfo->keyno & ~(LANE_COUNT - 1)) | lane;

        if (keyno == 0)
            keyno = LANE_COUNT;
        else if (keyno >= max_rows)
            keyno -= LANE_COUNT;

        tinfo->keyno = keyno;
    }
}

/*
 * replay_rollback --
 *     Called after a rollback.
 */
void
replay_rollback(TINFO *tinfo)
{
    if (!GV(RUNS_PREDICTABLE_REPLAY))
        return;

    /*
     * After a rollback, we don't give up our timestamp or our lane, we need to retry at the top of
     * the operations loop.
     */
    tinfo->replay_again = true;

    testutil_assert(tinfo->replay_ts != 0);
    testutil_assert(tinfo->lane != LANE_NONE);
    testutil_assert(g.lanes[tinfo->lane].in_use);
}

/*
 * replay_pause_after_rollback --
 *     Called after a rollback, allowing us to yield or pause.
 */
void
replay_pause_after_rollback(TINFO *tinfo, uint32_t ntries)
{
    uint64_t high, low, mid;

    if (!GV(RUNS_PREDICTABLE_REPLAY))
        return;

    /* Generally, the more behind we are, the less we want to wait. */
    low = replay_maximum_committed();
    high = g.timestamp;
    mid = high + low / 2;

    /* If we're in the furthest group behind, don't wait at all. */
    if (low + LANE_COUNT <= tinfo->replay_ts)
        return;

    /*
     * If we're in the last half, don't sleep. If we're in the front half, occasionally sleep.
     */
    if (tinfo->replay_ts < mid && ntries % 10 != 0)
        __wt_yield();
    else {
        /* Never sleep more than .1 seconds */
        __wt_sleep(0, ntries > 100 ? 100 * WT_THOUSAND : ntries * WT_THOUSAND);
    }
}
