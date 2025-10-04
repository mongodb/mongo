/**
 * Extends sharded_base_partitioned.js.
 *
 * Exercises the concurrent moveChunk operations, but each thread operates on its own set of
 * chunks.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {fsm} from "jstests/concurrency/fsm_libs/fsm.js";
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/chunks.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/sharded_partitioned/sharded_base_partitioned.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.iterations = 5;
    $config.threadCount = 5;

    $config.data.partitionSize = 100; // number of shard key values

    // Re-assign a chunk from this thread's partition to a random shard, and
    // verify that each node in the cluster affected by the moveChunk operation sees
    // the appropriate after-state regardless of whether the operation succeeded or failed.
    // Set `skipDocumentCountCheck` to true to skip verifying the number of documents in the chunk
    // before and after the migration.
    $config.states.moveChunk = function moveChunk(db, collName, connCache, skipDocumentCountCheck = false) {
        // Committing a chunk migration requires acquiring the global X lock on the CSRS primary.
        // This state function is unsafe to automatically run inside a multi-statement transaction
        // because it'll have left an idle transaction on the CSRS primary before attempting to run
        // the moveChunk command, which can lead to a hang.
        fsm.forceRunningOutsideTransaction(this);

        let ns = db[collName].getFullName();
        let config = connCache.rsConns.config;

        // Verify that more than one shard exists in the cluster. If only one shard existed,
        // there would be no way to move a chunk from one shard to another.
        let numShards = config.getDB("config").shards.find().itcount();
        let msg =
            "There must be more than one shard when performing a moveChunks operation\n" +
            "shards: " +
            tojson(config.getDB("config").shards.find().toArray());
        assert.gt(numShards, 1, msg);

        // Choose a random chunk in our partition to move.
        let chunk = this.getRandomChunkInPartition(collName, config);
        let fromShard = chunk.shard;

        // Choose a random shard to move the chunk to.
        let shardNames = Object.keys(connCache.shards);
        let destinationShards = shardNames.filter(function (shard) {
            if (shard !== fromShard) {
                return shard;
            }
        });
        let toShard = destinationShards[Random.randInt(destinationShards.length)];

        // Save the number of documents in this chunk's range found on the chunk's current shard
        // (the fromShard) before the moveChunk operation. This will be used to verify that the
        // number of documents in the chunk's range found on the _toShard_ after a _successful_
        // moveChunk operation is the same as numDocsBefore, or that the number of documents in the
        // chunk's range found on the _fromShard_ after a _failed_ moveChunk operation is the same
        // as numDocsBefore.
        // Choose the mongos randomly to distribute load.
        let numDocsBefore = ChunkHelper.getNumDocs(
            ChunkHelper.getRandomMongos(connCache.mongos),
            ns,
            chunk.min._id,
            chunk.max._id,
        );

        // Save the number of chunks before the moveChunk operation. This will be used
        // to verify that the number of chunks after the moveChunk operation remains the same.
        let numChunksBefore = ChunkHelper.getNumChunks(
            config,
            ns,
            this.partition.chunkLower,
            this.partition.chunkUpper,
        );

        // Randomly choose whether to wait for all documents on the fromShard
        // to be deleted before the moveChunk operation returns.
        let waitForDelete = Random.rand() < 0.5;

        // Use chunk_helper.js's moveChunk wrapper to tolerate acceptable failures
        // and to use a limited number of retries with exponential backoff.
        let bounds = [{_id: chunk.min._id}, {_id: chunk.max._id}];
        let moveChunkRes = ChunkHelper.moveChunk(db, collName, bounds, toShard, waitForDelete);
        let msgBase = "Result of moveChunk operation: " + tojson(moveChunkRes);

        // Verify that the fromShard and toShard have the correct after-state
        // (see comments below for specifics).
        let fromShardRSConn = connCache.rsConns.shards[fromShard];
        let toShardRSConn = connCache.rsConns.shards[toShard];
        let fromShardNumDocsAfter = ChunkHelper.getNumDocs(fromShardRSConn, ns, chunk.min._id, chunk.max._id);
        let toShardNumDocsAfter = ChunkHelper.getNumDocs(toShardRSConn, ns, chunk.min._id, chunk.max._id);
        // If the moveChunk operation succeeded, verify that the shard the chunk
        // was moved to returns all data for the chunk. If waitForDelete was true,
        // also verify that the shard the chunk was moved from returns no data for the chunk.
        if (moveChunkRes.ok) {
            const runningWithStepdowns = TestData.runningWithConfigStepdowns || TestData.runningWithShardStepdowns;

            if (waitForDelete && !runningWithStepdowns) {
                msg =
                    "moveChunk succeeded but original shard still had documents.\n" +
                    msgBase +
                    ", waitForDelete: " +
                    waitForDelete +
                    ", bounds: " +
                    tojson(bounds);
                assert.eq(fromShardNumDocsAfter, 0, msg);
            }
            if (!skipDocumentCountCheck) {
                msg =
                    "moveChunk succeeded but new shard did not contain all documents.\n" +
                    msgBase +
                    ", waitForDelete: " +
                    waitForDelete +
                    ", bounds: " +
                    tojson(bounds);
                assert.eq(toShardNumDocsAfter, numDocsBefore, msg);
            }
        }
        // If the moveChunk operation failed, verify that the shard the chunk was
        // originally on returns all data for the chunk, and the shard the chunk
        // was supposed to be moved to returns no data for the chunk.
        else {
            if (!skipDocumentCountCheck) {
                msg =
                    "moveChunk failed but original shard did not contain all documents.\n" +
                    msgBase +
                    ", waitForDelete: " +
                    waitForDelete +
                    ", bounds: " +
                    tojson(bounds);
                assert.eq(fromShardNumDocsAfter, numDocsBefore, msg);
            }
        }

        // Verify that all config servers have the correct after-state.
        // If the moveChunk operation succeeded, verify that the config updated the chunk's shard
        // with the toShard. If the operation failed, verify that the config kept the chunk's shard
        // as the fromShard.
        let chunkAfter = config.getDB("config").chunks.findOne({_id: chunk._id});
        msg = msgBase + "\nchunkBefore: " + tojson(chunk) + "\nchunkAfter: " + tojson(chunkAfter);
        if (moveChunkRes.ok) {
            msg = "moveChunk succeeded but chunk's shard was not new shard.\n" + msg;
            assert.eq(chunkAfter.shard, toShard, msg);
        } else {
            msg = "moveChunk failed but chunk's shard was not original shard.\n" + msg;
            assert.eq(chunkAfter.shard, fromShard, msg);
        }

        // Regardless of whether the operation succeeded or failed, verify that the number of chunks
        // in our partition stayed the same.
        let numChunksAfter = ChunkHelper.getNumChunks(config, ns, this.partition.chunkLower, this.partition.chunkUpper);
        msg = "Number of chunks in partition seen by config changed with moveChunk.\n" + msgBase;
        assert.eq(numChunksBefore, numChunksAfter, msg);

        // Verify that all mongos processes see the correct after-state on the shards and configs.
        // (see comments below for specifics).
        for (let mongos of connCache.mongos) {
            // Regardless of if the moveChunk operation succeeded or failed,
            // verify that each mongos sees as many documents in the chunk's
            // range after the move as there were before.
            if (!skipDocumentCountCheck) {
                let numDocsAfter = ChunkHelper.getNumDocs(mongos, ns, chunk.min._id, chunk.max._id);
                msg =
                    "Number of documents in range seen by mongos changed with moveChunk, range: " +
                    tojson(bounds) +
                    ".\n" +
                    msgBase;
                assert.eq(numDocsAfter, numDocsBefore, msg);
            }

            // If the moveChunk operation succeeded, verify that each mongos sees all data in the
            // chunk's range on only the toShard. If the operation failed, verify that each mongos
            // sees all data in the chunk's range on only the fromShard.
            let shardsForChunk = ChunkHelper.getShardsForRange(mongos, ns, chunk.min._id, chunk.max._id);
            msg = msgBase + "\nMongos find().explain() results for chunk: " + tojson(shardsForChunk);
            assert.eq(shardsForChunk.shards.length, 1, msg);
            if (moveChunkRes.ok) {
                msg = "moveChunk succeeded but chunk was not on new shard.\n" + msg;
                assert.eq(shardsForChunk.shards[0], toShard, msg);
            } else {
                msg = "moveChunk failed but chunk was not on original shard.\n" + msg;
                assert.eq(shardsForChunk.shards[0], fromShard, msg);
            }

            // If the moveChunk operation succeeded, verify that each mongos updated the chunk's
            // shard metadata with the toShard. If the operation failed, verify that each mongos
            // still sees the chunk's shard metadata as the fromShard.
            chunkAfter = mongos.getDB("config").chunks.findOne({_id: chunk._id});
            msg = msgBase + "\nchunkBefore: " + tojson(chunk) + "\nchunkAfter: " + tojson(chunkAfter);
            if (moveChunkRes.ok) {
                msg = "moveChunk succeeded but chunk's shard was not new shard.\n" + msg;
                assert.eq(chunkAfter.shard, toShard, msg);
            } else {
                msg = "moveChunk failed but chunk's shard was not original shard.\n" + msg;
                assert.eq(chunkAfter.shard, fromShard, msg);
            }
        }
    };

    $config.transitions = {init: {moveChunk: 1}, moveChunk: {moveChunk: 1}};

    return $config;
});
