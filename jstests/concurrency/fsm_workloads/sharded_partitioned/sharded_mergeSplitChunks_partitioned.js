/**
 * Extends sharded_mergeChunks_partitioned.js.
 *
 * Exercises concurrent moveChunk and mergeChunk operations, with each thread operating on its own
 * set of chunks.
 *
 * @tags: [requires_sharding, assumes_balancer_off, does_not_support_stepdowns]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/chunks.js";
import {minimumIterations} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/minimum_iterations.js";
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/sharded_partitioned/sharded_mergeChunks_partitioned.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.iterations = minimumIterations($config.states);
    $config.threadCount = 5;

    $config.data.partitionSize = 1100; // number of shard key values
    $config.data.chunkSize = 1;
    $config.data.prevChunkSize = 64;
    $config.data.bigString = "x".repeat(1024 * 128);

    $config.setup = function (db, collName, cluster) {
        // Overridden methods should usually call the corresponding
        // method on $super.
        $super.setup.apply(this, arguments);
    };

    $config.teardown = function (db, collName, cluster) {
        // $super.setup.teardown(this, arguments);
    };

    $config.data.setupAdditionalSplitPoints = function setupAdditionalSplitPoints(db, collName, partition) {
        let innerChunkSize = Math.floor(this.partitionSize / this.iterations);
        jsTestLog("innerChunkSize: " + innerChunkSize);
        for (let i = 0; i < this.iterations; ++i) {
            let splitPoint = (i + 1) * innerChunkSize + partition.lower;
            assert.commandWorked(ChunkHelper.splitChunkAtPoint(db, collName, splitPoint));
        }
    };

    // Split a random chunk in this thread's partition, and verify that each node
    // in the cluster affected by the splitChunk operation sees the appropriate
    // after-state regardless of whether the operation succeeded or failed.
    function doSplitChunk(db, collName, connCache) {
        let ns = db[collName].getFullName();
        let config = ChunkHelper.getPrimary(connCache.config);

        // Choose a random chunk in our partition to split.
        let chunk = this.getRandomChunkInPartition(collName, config);

        // Save the number of documents found in this chunk's range before the splitChunk
        // operation. This will be used to verify that the same number of documents in that
        // range are found after the splitChunk.
        // Choose the mongos randomly to distribute load.
        let numDocsBefore = ChunkHelper.getNumDocs(
            ChunkHelper.getRandomMongos(connCache.mongos),
            ns,
            chunk.min._id,
            chunk.max._id,
        );

        // Save the number of chunks before the splitChunk operation. This will be used
        // to verify that the number of chunks after a successful splitChunk increases
        // by one, or after a failed splitChunk stays the same.
        let numChunksBefore = ChunkHelper.getNumChunks(
            config,
            ns,
            this.partition.chunkLower,
            this.partition.chunkUpper,
        );

        assert(typeof chunk.min._id, "number");

        let splitChunkRes;
        if (chunk.min._id + 1 >= chunk.max._id || numDocsBefore <= 1) {
            jsTestLog("Chunk is not divisible " + tojson(chunk));
            splitChunkRes = {ok: false};
        } else {
            // Use chunk_helper.js's splitChunk wrapper to tolerate acceptable failures
            // and to use a limited number of retries with exponential backoff.
            let bounds = [{_id: chunk.min._id}, {_id: chunk.max._id}];
            splitChunkRes = ChunkHelper.splitChunkWithBounds(db, collName, bounds);
        }

        let msgBase = "Result of splitChunk operation: " + tojson(splitChunkRes);

        // Regardless of whether the splitChunk operation succeeded or failed,
        // verify that the shard the original chunk was on returns all data for the chunk.
        let shardPrimary = ChunkHelper.getPrimary(connCache.shards[chunk.shard]);
        let shardNumDocsAfter = ChunkHelper.getNumDocs(shardPrimary, ns, chunk.min._id, chunk.max._id);
        let msg = "Shard does not have same number of documents after splitChunk.\n" + msgBase;
        assert.eq(shardNumDocsAfter, numDocsBefore, msg);

        // Verify that all config servers have the correct after-state.
        // (see comments below for specifics).
        for (let conn of connCache.config) {
            let res = conn.adminCommand({hello: 1});
            assert.commandWorked(res);
            if (res.isWritablePrimary) {
                // If the splitChunk operation succeeded, verify that there are now
                // two chunks between the old chunk's lower and upper bounds.
                // If the operation failed, verify that there is still only one chunk
                // between the old chunk's lower and upper bounds.
                let numChunksBetweenOldChunksBounds = ChunkHelper.getNumChunks(conn, ns, chunk.min._id, chunk.max._id);
                if (splitChunkRes.ok) {
                    msg =
                        "splitChunk succeeded but the config does not see exactly 2 chunks " +
                        "between the chunk bounds.\n" +
                        msgBase;
                    assert.eq(numChunksBetweenOldChunksBounds, 2, msg);
                } else {
                    msg =
                        "splitChunk failed but the config does not see exactly 1 chunk between " +
                        "the chunk bounds.\n" +
                        msgBase;
                    assert.eq(numChunksBetweenOldChunksBounds, 1, msg);
                }

                // If the splitChunk operation succeeded, verify that the total number
                // of chunks in our partition has increased by 1. If it failed, verify
                // that it has stayed the same.
                let numChunksAfter = ChunkHelper.getNumChunks(
                    config,
                    ns,
                    this.partition.chunkLower,
                    this.partition.chunkUpper,
                );
                if (splitChunkRes.ok) {
                    msg =
                        "splitChunk succeeded but the config does nnot see exactly 1 more " +
                        "chunk between the chunk bounds.\n" +
                        msgBase;
                    assert.eq(numChunksAfter, numChunksBefore + 1, msg);
                } else {
                    msg =
                        "splitChunk failed but the config does not see the same number " +
                        "of chunks between the chunk bounds.\n" +
                        msgBase;
                    assert.eq(numChunksAfter, numChunksBefore, msg);
                }
            }
        }

        // Verify that all mongos processes see the correct after-state on the shards and configs.
        // (see comments below for specifics).
        for (let mongos of connCache.mongos) {
            // Regardless of if the splitChunk operation succeeded or failed, verify that each
            // mongos sees as many documents in the chunk's range after the move as there were
            // before.
            let numDocsAfter = ChunkHelper.getNumDocs(mongos, ns, chunk.min._id, chunk.max._id);

            msg = "Mongos does not see same number of documents after splitChunk.\n" + msgBase;
            assert.eq(numDocsAfter, numDocsBefore, msgBase);

            // Regardless of if the splitChunk operation succeeded or failed,
            // verify that each mongos sees all data in the original chunk's
            // range only on the shard the original chunk was on.
            let shardsForChunk = ChunkHelper.getShardsForRange(mongos, ns, chunk.min._id, chunk.max._id);
            msg =
                "Mongos does not see exactly 1 shard for chunk after splitChunk.\n" +
                msgBase +
                "\n" +
                "Mongos find().explain() results for chunk: " +
                tojson(shardsForChunk);
            assert.eq(shardsForChunk.shards.length, 1, msg);

            msg =
                "Mongos sees different shard for chunk than chunk does after splitChunk.\n" +
                msgBase +
                "\n" +
                "Mongos find().explain() results for chunk: " +
                tojson(shardsForChunk);
            assert.eq(shardsForChunk.shards[0], chunk.shard, msg);

            // If the splitChunk operation succeeded, verify that the mongos sees two chunks between
            // the old chunk's lower and upper bounds. If the operation failed, verify that the
            // mongos still only sees one chunk between the old chunk's lower and upper bounds.
            let numChunksBetweenOldChunksBounds = ChunkHelper.getNumChunks(mongos, ns, chunk.min._id, chunk.max._id);
            if (splitChunkRes.ok) {
                msg =
                    "splitChunk succeeded but the mongos does not see exactly 2 chunks " +
                    "between the chunk bounds.\n" +
                    msgBase;
                assert.eq(numChunksBetweenOldChunksBounds, 2, msg);
            } else {
                msg =
                    "splitChunk failed but the mongos does not see exactly 1 chunk between " +
                    "the chunk bounds.\n" +
                    msgBase;
                assert.eq(numChunksBetweenOldChunksBounds, 1, msg);
            }

            // If the splitChunk operation succeeded, verify that the total number of chunks in our
            // partition has increased by 1. If it failed, verify that it has stayed the same.
            let numChunksAfter = ChunkHelper.getNumChunks(
                mongos,
                ns,
                this.partition.chunkLower,
                this.partition.chunkUpper,
            );
            if (splitChunkRes.ok) {
                msg =
                    "splitChunk succeeded but the mongos does nnot see exactly 1 more " +
                    "chunk between the chunk bounds.\n" +
                    msgBase;
                assert.eq(numChunksAfter, numChunksBefore + 1, msg);
            } else {
                msg =
                    "splitChunk failed but the mongos does not see the same number " +
                    "of chunks between the chunk bounds.\n" +
                    msgBase;
                assert.eq(numChunksAfter, numChunksBefore, msg);
            }
        }
    }

    $config.states.splitChunk = function (db, collName, connCache) {
        jsTestLog("Running splitChunk");
        doSplitChunk.apply(this, [db, collName, connCache]);
    };

    $config.transitions = uniformDistTransitions($config.states);

    return $config;
});
