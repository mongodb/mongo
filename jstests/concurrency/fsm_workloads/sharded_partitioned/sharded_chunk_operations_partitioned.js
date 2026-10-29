/**
 * Extends sharded_base_partitioned.js.
 *
 * Runs chunk operations (moveChunk, splitChunk, mergeChunks) on many threads at once, and checks
 * after each one that it really happened.
 *
 * How it works:
 *   - Each thread owns its own `_id` range, filled at setup, and only touches chunks that fall
 *     inside that range. Ranges never overlap and the balancer is off, so a thread is the only one
 *     that changes the chunks in its range.
 *   - Because nothing else changes those chunks, reading config.chunks right after a command shows
 *     that command's result. So each successful operation is checked: a split leaves two chunks
 *     that cover the old range, a merge leaves one chunk with the merged bounds, and a move changes
 *     the chunk's shard.
 *   - Chunk operations never add or remove documents, so verifyPartition also checks that the range
 *     still holds exactly its `_id` values, with none lost, duplicated, or left behind.
 *   - Each operation's outcome (committed vs. lost to a concurrent-op race) is counted per type and
 *     printed at the end, and teardown fails if nothing committed, so a run that exercised almost
 *     nothing cannot pass silently.
 *
 *              <== t0 partition ==>  <== t1 partition ==>  <== t2 partition ==>
 *    _id:     [ 0 ............ 99 )[ 100 .......... 199 )[ 200 .......... 299 )
 *    setup:   [ one full chunk    )[ one full chunk     )[ one full chunk     )
 *
 * The threads work on the same collection: thread A reshapes the chunks of one range while thread B
 * reshapes another range. They both contend for the collection critical section and the
 * ActiveMigrationsRegistry even though they touch different chunks. This is the interaction the
 * chunk-operation serialization must keep correct and free of deadlocks.
 *
 * mergeAllChunksOnShard is left out: it works per shard, not per range, so it cannot be kept inside
 * one thread's range and its effect cannot be checked per thread. That command is covered by
 * sharded_chunk_operations_with_crud_partitioned.js.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {fsm} from "jstests/concurrency/fsm_libs/fsm.js";
import {ChunkOperationHelpers} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunk_operation_helpers.js";
import {Operation} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunk_operation_errors.js";
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {findFirstBatch} from "jstests/concurrency/fsm_workload_helpers/stepdown_suite_helpers.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/sharded_partitioned/sharded_base_partitioned.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 30;

    // Each thread's partition spans `partitionSize` shard-key values, all populated at setup so the
    // partition always contains exactly `partitionSize` documents regardless of how its chunks are
    // reshaped or where they are placed.
    $config.data.partitionSize = 100;

    // Install the ChunkOperationHelpers.
    Object.assign($config.data, ChunkOperationHelpers);
    $config.data.trackedChunkOps = [
        Operation.MoveChunk,
        Operation.SplitChunk,
        Operation.MergeChunks,
    ];

    // Return chunks whose whole range lies within [minBound, maxBound], sorted by min._id.
    $config.data.chunksCoveringRange = function (config, ns, minBound, maxBound) {
        return findChunksUtil
            .findChunksByNs(config.getDB("config"), ns)
            .sort({"min._id": 1})
            .toArray()
            .filter(
                (c) =>
                    bsonWoCompare(c.min, {_id: minBound}) >= 0 &&
                    bsonWoCompare(c.max, {_id: maxBound}) <= 0,
            );
    };

    // --------------------------------------------------------------------------------------
    // Per-operation effect verification.
    //
    // A thread is the only writer of chunk metadata within its partition and the balancer is off, so
    // a read of config.chunks issued synchronously right after a committed command observes exactly
    // that command's effect. These are hard asserts: they run only after success, and a failure here
    // means the operation did not apply as claimed, not a tolerable race.
    // --------------------------------------------------------------------------------------

    // split [lo, hi) -> exactly two contiguous chunks tiling [lo, hi), both still on `shard`.
    $config.data.verifySplitApplied = function (config, ns, lo, hi, shard, opDesc) {
        const chunksWithin = this.chunksCoveringRange(config, ns, lo, hi);
        assert.eq(chunksWithin.length, 2, `split not applied: ${opDesc}`, {chunksWithin});
        assert.eq(
            bsonWoCompare(chunksWithin[0].min, {_id: lo}),
            0,
            `split lower bound moved: ${opDesc}`,
            {chunksWithin},
        );
        assert.eq(
            bsonWoCompare(chunksWithin[1].max, {_id: hi}),
            0,
            `split upper bound moved: ${opDesc}`,
            {chunksWithin},
        );
        assert.eq(
            bsonWoCompare(chunksWithin[0].max, chunksWithin[1].min),
            0,
            `split left a gap/overlap: ${opDesc}`,
            {chunksWithin},
        );
        assert.eq(chunksWithin[0].shard, shard, `split moved a chunk: ${opDesc}`, {chunksWithin});
        assert.eq(chunksWithin[1].shard, shard, `split moved a chunk: ${opDesc}`, {chunksWithin});
    };

    // merge [aMin, bMax) -> exactly one chunk with those bounds, on `shard`.
    $config.data.verifyMergeApplied = function (config, ns, aMin, bMax, shard, opDesc) {
        const chunksWithin = this.chunksCoveringRange(config, ns, aMin._id, bMax._id);
        assert.eq(chunksWithin.length, 1, `merge not applied: ${opDesc}`, {chunksWithin});
        assert.eq(
            bsonWoCompare(chunksWithin[0].min, aMin),
            0,
            `merge lower bound wrong: ${opDesc}`,
            {
                chunksWithin,
            },
        );
        assert.eq(
            bsonWoCompare(chunksWithin[0].max, bMax),
            0,
            `merge upper bound wrong: ${opDesc}`,
            {
                chunksWithin,
            },
        );
        assert.eq(chunksWithin[0].shard, shard, `merge result on wrong shard: ${opDesc}`, {
            chunksWithin,
        });
    };

    // move [min, max) -> that exact chunk now reports shard == toShard.
    $config.data.verifyMoveApplied = function (config, ns, chunkMin, chunkMax, toShard, opDesc) {
        const chunksWithin = this.chunksCoveringRange(
            config,
            ns,
            chunkMin._id,
            chunkMax._id,
        ).filter(
            (c) => bsonWoCompare(c.min, chunkMin) === 0 && bsonWoCompare(c.max, chunkMax) === 0,
        );
        assert.eq(chunksWithin.length, 1, `move target chunk not found: ${opDesc}`, {chunksWithin});
        assert.eq(chunksWithin[0].shard, toShard, `move not applied: ${opDesc}`, {chunksWithin});
    };

    // Returns this thread's chunks, sorted by min._id. A chunk belongs to the partition when its
    // whole range lies within the partition bounds.
    $config.data.chunksInPartition = function (configConn, ns) {
        const lo = this.partition.isLowChunk ? MinKey : this.partition.lower;
        const hi = this.partition.isHighChunk ? MaxKey : this.partition.upper;
        return this.chunksCoveringRange(configConn, ns, lo, hi);
    };

    // --------------------------------------------------------------------------------------
    // Setup and init. The base setup populates every partition fully ([lower, upper)) and splits it
    // into its own chunk. Init only records the partition; the single-chunk invariant is skipped
    // because the balancer and add/remove-shard reshape chunks independently of the thread.
    // --------------------------------------------------------------------------------------

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.call(this, db, collName, cluster);
        this.initChunkOpStats(db, collName);
    };

    $config.teardown = function teardown(db, collName, cluster) {
        this.dumpChunkOpStats(db, collName);
        $super.teardown.call(this, db, collName, cluster);
    };

    $config.states.init = function initState(db, collName, connCache) {
        $super.states.init.call(this, db, collName, connCache, true /* skipNumChunksCheck */);
        jsTest.log.info("Init partition", {
            tid: this.tid,
            partition: this.partition,
            partitionSize: this.partitionSize,
        });
    };

    // --------------------------------------------------------------------------------------
    // Chunk operations, each confined to this thread's partition.
    // --------------------------------------------------------------------------------------

    $config.states.moveChunk = function moveChunkState(db, collName, connCache) {
        fsm.forceRunningOutsideTransaction(this);

        const config = connCache.rsConns.config;
        const numShards = findFirstBatch(config.getDB("config"), "shards", {}, 100).length;
        assert.gte(numShards, 2, "moveChunk requires at least 2 shards");

        const chunk = this.getRandomChunkInPartition(collName, config);
        assert(chunk, `tid ${this.tid}: no chunk found in partition for moveChunk`);

        this.runMoveChunk(db, collName, config, chunk, (ns, c, toShard, opDesc) =>
            this.verifyMoveApplied(config, ns, c.min, c.max, toShard, opDesc),
        );
    };

    $config.states.splitChunk = function splitChunkState(db, collName, connCache) {
        fsm.forceRunningOutsideTransaction(this);

        const config = connCache.rsConns.config;
        const chunk = this.getRandomChunkInPartition(collName, config);
        assert(chunk, `tid ${this.tid}: no chunk found in partition for splitChunk`);

        this.runSplitChunk(db, collName, chunk, (ns, lo, hi, shard, opDesc) =>
            this.verifySplitApplied(config, ns, lo, hi, shard, opDesc),
        );
    };

    $config.states.mergeChunks = function mergeChunksState(db, collName, connCache) {
        fsm.forceRunningOutsideTransaction(this);

        const ns = db[collName].getFullName();
        const config = connCache.rsConns.config;
        const pairs = this.findMergeableAdjacentPairs(this.chunksInPartition(config, ns));
        if (pairs.length === 0) {
            return;
        }
        const pair = pairs[Random.randInt(pairs.length)];
        this.runMergeChunks(db, collName, pair, (ns2, aMin, bMax, shard, opDesc) =>
            this.verifyMergeApplied(config, ns2, aMin, bMax, shard, opDesc),
        );
    };

    // --------------------------------------------------------------------------------------
    // Verification state.
    //
    // Chunk operations never add or remove documents, so the partition must always contain exactly
    // the values [lower, upper) that setup inserted, no matter how its chunks were reshaped or
    // placed. Reading through mongos exercises routing (the router refreshes and retries on
    // StaleConfig), so a correct system always returns the full set.
    // --------------------------------------------------------------------------------------

    $config.states.verifyPartition = function verifyPartitionState(db, collName, connCache) {
        const docs = findFirstBatch(
            db,
            collName,
            {_id: {$gte: this.partition.lower, $lt: this.partition.upper}},
            1e6,
        );

        const seen = new Set();
        for (const doc of docs) {
            assert(!seen.has(doc._id), `duplicate _id ${doc._id} in partition find result`);
            seen.add(doc._id);
        }

        assert.eq(
            docs.length,
            this.partitionSize,
            `partition [${this.partition.lower}, ${this.partition.upper}) doc count mismatch:` +
                ` expected ${this.partitionSize}, got ${docs.length}`,
        );
        for (let i = this.partition.lower; i < this.partition.upper; ++i) {
            assert(seen.has(i), `partition is missing _id ${i}`);
        }
    };

    $config.transitions = uniformDistTransitions($config.states);
    return $config;
});
