/**
 * Extends sharded_base_partitioned.js.
 *
 * Exercises the concurrent moveChunk operations, with each thread operating on its own set of
 * chunks.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/chunks.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/sharded_partitioned/sharded_base_partitioned.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.iterations = 8;
    $config.threadCount = 5;

    $config.data.partitionSize = 100; // number of shard key values

    // Create at least as many additional split points in this thread's partition as there
    // will be iterations (to accommodate as many mergeChunks operations in this thread's
    // partition as iterations).
    //
    // This is done in setup rather than in a mergeChunk-specific init state after the
    // sharded_base_partitioned.js init state because the states are multi-threaded:
    // since the mergeChunks operation used to create the chunks within each partition is not
    // guaranteed to succeed (it can fail if another concurrent chunk operation is in progress),
    // it is much more complicated to do this setup step in a multi-threaded context.
    $config.data.setupAdditionalSplitPoints = function setupAdditionalSplitPoints(db, collName, partition) {
        // Add as many additional split points as iterations.
        // Define the inner chunk size as the max size of the range of shard key
        // values in each inner chunk within the thread partition as the largest
        // whole number that allows for as many inner chunks as iterations without
        // exceeding partitionSize.
        //
        // Diagram for partitionSize = 5, iterations = 4 ==> innerChunkSize = 1:
        // [----------] ==> [-|-|-|-|-]
        // 0          5     0 1 2 3 4 5
        //
        // Diagram for partitionSize = 5, iterations = 2 ==> innerChunkSize = 2:
        // [----------] ==> [-|--|--]
        // 0          5     0 1  3  5
        //
        // Diagram for partitionSize = 5, iterations = 1 ==> innerChunkSize = 5:
        // [----------] ==> [-|----]
        // 0          5     0 1    5
        let innerChunkSize = Math.floor(this.partitionSize / this.iterations);
        for (let i = 0; i < this.iterations; ++i) {
            let splitPoint = (i + 1) * innerChunkSize + partition.lower;
            assert.commandWorked(ChunkHelper.splitChunkAtPoint(db, collName, splitPoint));
        }
    };

    // Override sharded_base_partitioned's init state to prevent the default check
    // that only 1 chunk is in our partition and to instead check that there are
    // at least as many chunks in our partition as iterations.
    $config.states.init = function init(db, collName, connCache) {
        // Inform this thread about its partition.
        // Each thread has tid in range 0..(n-1) where n is the number of threads.
        this.partition = this.makePartition(db[collName].getFullName(), this.tid, this.partitionSize);
        Object.freeze(this.partition);

        let config = ChunkHelper.getPrimary(connCache.config);

        let numChunksInPartition = ChunkHelper.getNumChunks(
            config,
            db[collName].getFullName(),
            this.partition.chunkLower,
            this.partition.chunkUpper,
        );

        // Verify that there is at least one chunk in our partition and that
        // there are at least as many chunks in our partition as iterations.
        assert.gte(numChunksInPartition, 1, "should be at least one chunk in each thread's partition.");
        assert.gt(
            numChunksInPartition,
            this.iterations,
            "should be more chunks in each thread's partition " +
                "than iterations in order to accomodate that many mergeChunks.",
        );
    };

    // Merge a random chunk in this thread's partition with its upper neighbor.
    $config.states.mergeChunks = function mergeChunks(db, collName, connCache) {
        let ns = db[collName].getFullName();
        let config = ChunkHelper.getPrimary(connCache.config);

        let chunk1, chunk2;
        let configDB = config.getDB("config");

        // Skip this iteration if our data partition contains less than 2 chunks.
        if (
            findChunksUtil
                .findChunksByNs(configDB, ns, {
                    "min._id": {$gte: this.partition.lower},
                    "max._id": {$lte: this.partition.upper},
                })
                .itcount() < 2
        ) {
            return;
        }

        // Grab a chunk and its upper neighbor.
        chunk1 = this.getRandomChunkInPartition(collName, config);
        // If we randomly chose the last chunk, choose the one before it.
        if (chunk1.max._id === this.partition.chunkUpper) {
            chunk1 = findChunksUtil.findOneChunkByNs(configDB, ns, {"max._id": chunk1.min._id});
        }
        chunk2 = findChunksUtil.findOneChunkByNs(configDB, ns, {"min._id": chunk1.max._id});

        // Save the number of documents found in these two chunks' ranges before the mergeChunks
        // operation. This will be used to verify that the same number of documents in that
        // range are still found after the mergeChunks.
        // Choose the mongos randomly to distribute load.
        let numDocsBefore = ChunkHelper.getNumDocs(
            ChunkHelper.getRandomMongos(connCache.mongos),
            ns,
            chunk1.min._id,
            chunk2.max._id,
        );

        // If the second chunk is not on the same shard as the first, move it,
        // because mergeChunks requires the chunks being merged to be on the same shard.
        if (chunk2.shard !== chunk1.shard) {
            ChunkHelper.moveChunk(db, collName, chunk2.min._id, chunk1.shard, true);
        }

        // Verify that no docs were lost in the moveChunk.
        let shardPrimary = ChunkHelper.getPrimary(connCache.shards[chunk1.shard]);
        let shardNumDocsAfter = ChunkHelper.getNumDocs(shardPrimary, ns, chunk1.min._id, chunk2.max._id);
        let msg = "Chunk1's shard should contain all documents after mergeChunks.\n" + msgBase;
        assert.eq(shardNumDocsAfter, numDocsBefore, msg);

        // Save the number of chunks before the mergeChunks operation. This will be used
        // to verify that the number of chunks after a successful mergeChunks decreases
        // by one, or after a failed mergeChunks stays the same.
        let numChunksBefore = ChunkHelper.getNumChunks(
            config,
            ns,
            this.partition.chunkLower,
            this.partition.chunkUpper,
        );

        // Use chunk_helper.js's mergeChunks wrapper to tolerate acceptable failures
        // and to use a limited number of retries with exponential backoff.
        let bounds = [{_id: chunk1.min._id}, {_id: chunk2.max._id}];
        let mergeChunksRes = ChunkHelper.mergeChunks(db, collName, bounds);
        let chunks = ChunkHelper.getChunks(config, ns, this.partition.chunkLower, this.partition.chunkUpper);
        var msgBase = tojson({
            mergeChunksResult: mergeChunksRes,
            chunksInPartition: chunks,
            chunk1: chunk1,
            chunk2: chunk2,
        });

        // Regardless of whether the mergeChunks operation succeeded or failed,
        // verify that the shard chunk1 was on returns all data for the chunk.
        shardPrimary = ChunkHelper.getPrimary(connCache.shards[chunk1.shard]);
        shardNumDocsAfter = ChunkHelper.getNumDocs(shardPrimary, ns, chunk1.min._id, chunk2.max._id);
        msg = "Chunk1's shard should contain all documents after mergeChunks.\n" + msgBase;
        assert.eq(shardNumDocsAfter, numDocsBefore, msg);

        // Verify that all config servers have the correct after-state.
        // (see comments below for specifics).
        for (let conn of connCache.config) {
            let res = conn.adminCommand({hello: 1});
            assert.commandWorked(res);
            if (res.isWritablePrimary) {
                // If the mergeChunks operation succeeded, verify that there is now one chunk
                // between the original chunks' lower and upper bounds. If the operation failed,
                // verify that there are still two chunks between the original chunks' lower and
                // upper bounds.
                let numChunksBetweenOldChunksBounds = ChunkHelper.getNumChunks(
                    conn,
                    ns,
                    chunk1.min._id,
                    chunk2.max._id,
                );
                if (mergeChunksRes.ok) {
                    msg =
                        "mergeChunks succeeded but config does not see exactly 1 chunk between " +
                        "the chunk bounds.\n" +
                        msgBase;
                    assert.eq(numChunksBetweenOldChunksBounds, 1, msg);
                } else {
                    msg =
                        "mergeChunks failed but config does not see exactly 2 chunks between " +
                        "the chunk bounds.\n" +
                        msgBase;
                    assert.eq(numChunksBetweenOldChunksBounds, 2, msg);
                }

                // If the mergeChunks operation succeeded, verify that the total number
                // of chunks in our partition has decreased by 1. If it failed, verify
                // that it has stayed the same.
                let numChunksAfter = ChunkHelper.getNumChunks(
                    config,
                    ns,
                    this.partition.chunkLower,
                    this.partition.chunkUpper,
                );
                if (mergeChunksRes.ok) {
                    msg =
                        "mergeChunks succeeded but config does not see exactly 1 fewer chunks " +
                        "between the chunk bounds than before.\n" +
                        msgBase;
                    assert.eq(numChunksAfter, numChunksBefore - 1, msg);
                } else {
                    msg =
                        "mergeChunks failed but config sees a different number of chunks " +
                        "between the chunk bounds.\n" +
                        msgBase;
                    assert.eq(numChunksAfter, numChunksBefore, msg);
                }
            }
        }

        // Verify that all mongos processes see the correct after-state on the shards and configs.
        // (see comments below for specifics).
        for (let mongos of connCache.mongos) {
            // Regardless of if the mergeChunks operation succeeded or failed, verify that each
            // mongos sees as many documents in the original chunks' range after the move as there
            // were before.
            let numDocsAfter = ChunkHelper.getNumDocs(mongos, ns, chunk1.min._id, chunk2.max._id);
            msg =
                "Mongos sees a different amount of documents between chunk bounds after " + "mergeChunks.\n" + msgBase;
            assert.eq(numDocsAfter, numDocsBefore, msg);

            // Regardless of if the mergeChunks operation succeeded or failed, verify that each
            // mongos sees all data in the original chunks' range only on the shard the original
            // chunk was on.
            let shardsForChunk = ChunkHelper.getShardsForRange(mongos, ns, chunk1.min._id, chunk2.max._id);
            msg =
                "Mongos does not see exactly 1 shard for chunk after mergeChunks.\n" +
                msgBase +
                "\n" +
                "Mongos find().explain() results for chunk: " +
                tojson(shardsForChunk);
            assert.eq(shardsForChunk.shards.length, 1, msg);
            msg =
                "Mongos sees different shard for chunk than chunk does after mergeChunks.\n" +
                msgBase +
                "\n" +
                "Mongos find().explain() results for chunk: " +
                tojson(shardsForChunk);
            assert.eq(shardsForChunk.shards[0], chunk1.shard, msg);

            // If the mergeChunks operation succeeded, verify that the mongos sees one chunk between
            // the original chunks' lower and upper bounds. If the operation failed, verify that the
            // mongos still sees two chunks between the original chunks' lower and upper bounds.
            let numChunksBetweenOldChunksBounds = ChunkHelper.getNumChunks(mongos, ns, chunk1.min._id, chunk2.max._id);
            if (mergeChunksRes.ok) {
                msg =
                    "mergeChunks succeeded but mongos does not see exactly 1 chunk between " +
                    "the chunk bounds.\n" +
                    msgBase;
                assert.eq(numChunksBetweenOldChunksBounds, 1, msg);
            } else {
                msg =
                    "mergeChunks failed but mongos does not see exactly 2 chunks between " +
                    "the chunk bounds.\n" +
                    msgBase;
                assert.eq(numChunksBetweenOldChunksBounds, 2, msg);
            }

            // If the mergeChunks operation succeeded, verify that the mongos sees that the total
            // number of chunks in our partition has decreased by 1. If it failed, verify that it
            // has stayed the same.
            let numChunksAfter = ChunkHelper.getNumChunks(
                mongos,
                ns,
                this.partition.chunkLower,
                this.partition.chunkUpper,
            );
            if (mergeChunksRes.ok) {
                msg =
                    "mergeChunks succeeded but mongos does not see exactly 1 fewer chunks " +
                    "between the chunk bounds.\n" +
                    msgBase;
                assert.eq(numChunksAfter, numChunksBefore - 1, msg);
            } else {
                msg =
                    "mergeChunks failed but mongos does not see the same number of chunks " +
                    "between the chunk bounds.\n" +
                    msgBase;
                assert.eq(numChunksAfter, numChunksBefore, msg);
            }
        }
    };

    $config.transitions = {init: {mergeChunks: 1}, mergeChunks: {mergeChunks: 1}};

    return $config;
});
