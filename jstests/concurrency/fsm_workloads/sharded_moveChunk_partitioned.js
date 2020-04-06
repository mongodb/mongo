'use strict';

/**
 * Extends sharded_base_partitioned.js.
 *
 * Exercises the concurrent moveChunk operations, but each thread operates on its own set of
 * chunks.
 *
 * @tags: [requires_sharding, assumes_balancer_off, assumes_autosplit_off]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');                // for extendWorkload
load('jstests/concurrency/fsm_workloads/sharded_base_partitioned.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.iterations = 5;
    $config.threadCount = 5;

    $config.data.partitionSize = 100;  // number of shard key values

    // Re-assign a chunk from this thread's partition to a random shard, and
    // verify that each node in the cluster affected by the moveChunk operation sees
    // the appropriate after-state regardless of whether the operation succeeded or failed.
    $config.states.moveChunk = function moveChunk(db, collName, connCache) {
        // Committing a chunk migration requires acquiring the global X lock on the CSRS primary.
        // This state function is unsafe to automatically run inside a multi-statement transaction
        // because it'll have left an idle transaction on the CSRS primary before attempting to run
        // the moveChunk command, which can lead to a hang.
        fsm.forceRunningOutsideTransaction(this);

        var ns = db[collName].getFullName();
        var config = connCache.rsConns.config;

        // Verify that more than one shard exists in the cluster. If only one shard existed,
        // there would be no way to move a chunk from one shard to another.
        var numShards = config.getDB('config').shards.find().itcount();
        var msg = 'There must be more than one shard when performing a moveChunks operation\n' +
            'shards: ' + tojson(config.getDB('config').shards.find().toArray());
        assertAlways.gt(numShards, 1, msg);

        // Choose a random chunk in our partition to move.
        var chunk = this.getRandomChunkInPartition(collName, config);
        var fromShard = chunk.shard;

        // Choose a random shard to move the chunk to.
        var shardNames = Object.keys(connCache.shards);
        var destinationShards = shardNames.filter(function(shard) {
            if (shard !== fromShard) {
                return shard;
            }
        });
        var toShard = destinationShards[Random.randInt(destinationShards.length)];

        // Save the number of documents in this chunk's range found on the chunk's current shard
        // (the fromShard) before the moveChunk operation. This will be used to verify that the
        // number of documents in the chunk's range found on the _toShard_ after a _successful_
        // moveChunk operation is the same as numDocsBefore, or that the number of documents in the
        // chunk's range found on the _fromShard_ after a _failed_ moveChunk operation is the same
        // as numDocsBefore.
        // Choose the mongos randomly to distribute load.
        var numDocsBefore = ChunkHelper.getNumDocs(
            ChunkHelper.getRandomMongos(connCache.mongos), ns, chunk.min._id, chunk.max._id);

        // Save the number of chunks before the moveChunk operation. This will be used
        // to verify that the number of chunks after the moveChunk operation remains the same.
        var numChunksBefore = ChunkHelper.getNumChunks(
            config, ns, this.partition.chunkLower, this.partition.chunkUpper);

        // Randomly choose whether to wait for all documents on the fromShard
        // to be deleted before the moveChunk operation returns.
        var waitForDelete = Random.rand() < 0.5;

        // Use chunk_helper.js's moveChunk wrapper to tolerate acceptable failures
        // and to use a limited number of retries with exponential backoff.
        var bounds = [{_id: chunk.min._id}, {_id: chunk.max._id}];
        var moveChunkRes = ChunkHelper.moveChunk(db, collName, bounds, toShard, waitForDelete);
        var msgBase = 'Result of moveChunk operation: ' + tojson(moveChunkRes);

        // Verify that the fromShard and toShard have the correct after-state
        // (see comments below for specifics).
        var fromShardRSConn = connCache.rsConns.shards[fromShard];
        var toShardRSConn = connCache.rsConns.shards[toShard];
        var fromShardNumDocsAfter =
            ChunkHelper.getNumDocs(fromShardRSConn, ns, chunk.min._id, chunk.max._id);
        var toShardNumDocsAfter =
            ChunkHelper.getNumDocs(toShardRSConn, ns, chunk.min._id, chunk.max._id);
        // If the moveChunk operation succeeded, verify that the shard the chunk
        // was moved to returns all data for the chunk. If waitForDelete was true,
        // also verify that the shard the chunk was moved from returns no data for the chunk.
        if (moveChunkRes.ok) {
            const runningWithStepdowns =
                TestData.runningWithConfigStepdowns || TestData.runningWithShardStepdowns;

            // TODO SERVER-46669: The moveChunk command can succeed without waiting for the range
            // deletion to complete if the replica set shard primary steps down.
            if (waitForDelete && !runningWithStepdowns) {
                msg = 'moveChunk succeeded but original shard still had documents.\n' + msgBase +
                    ', waitForDelete: ' + waitForDelete + ', bounds: ' + tojson(bounds);
                assertWhenOwnColl.eq(fromShardNumDocsAfter, 0, msg);
            }
            msg = 'moveChunk succeeded but new shard did not contain all documents.\n' + msgBase +
                ', waitForDelete: ' + waitForDelete + ', bounds: ' + tojson(bounds);
            assertWhenOwnColl.eq(toShardNumDocsAfter, numDocsBefore, msg);
        }
        // If the moveChunk operation failed, verify that the shard the chunk was
        // originally on returns all data for the chunk, and the shard the chunk
        // was supposed to be moved to returns no data for the chunk.
        else {
            msg = 'moveChunk failed but original shard did not contain all documents.\n' + msgBase +
                ', waitForDelete: ' + waitForDelete + ', bounds: ' + tojson(bounds);
            assertWhenOwnColl.eq(fromShardNumDocsAfter, numDocsBefore, msg);
        }

        // Verify that all config servers have the correct after-state.
        // If the moveChunk operation succeeded, verify that the config updated the chunk's shard
        // with the toShard. If the operation failed, verify that the config kept the chunk's shard
        // as the fromShard.
        var chunkAfter = config.getDB('config').chunks.findOne({_id: chunk._id});
        var msg =
            msgBase + '\nchunkBefore: ' + tojson(chunk) + '\nchunkAfter: ' + tojson(chunkAfter);
        if (moveChunkRes.ok) {
            msg = "moveChunk succeeded but chunk's shard was not new shard.\n" + msg;
            assertWhenOwnColl.eq(chunkAfter.shard, toShard, msg);
        } else {
            msg = "moveChunk failed but chunk's shard was not original shard.\n" + msg;
            assertWhenOwnColl.eq(chunkAfter.shard, fromShard, msg);
        }

        // Regardless of whether the operation succeeded or failed, verify that the number of chunks
        // in our partition stayed the same.
        var numChunksAfter = ChunkHelper.getNumChunks(
            config, ns, this.partition.chunkLower, this.partition.chunkUpper);
        msg = 'Number of chunks in partition seen by config changed with moveChunk.\n' + msgBase;
        assertWhenOwnColl.eq(numChunksBefore, numChunksAfter, msg);

        // Verify that all mongos processes see the correct after-state on the shards and configs.
        // (see comments below for specifics).
        for (var mongos of connCache.mongos) {
            // Regardless of if the moveChunk operation succeeded or failed,
            // verify that each mongos sees as many documents in the chunk's
            // range after the move as there were before.
            var numDocsAfter = ChunkHelper.getNumDocs(mongos, ns, chunk.min._id, chunk.max._id);
            msg = 'Number of documents in range seen by mongos changed with moveChunk, range: ' +
                tojson(bounds) + '.\n' + msgBase;
            assertWhenOwnColl.eq(numDocsAfter, numDocsBefore, msg);

            // If the moveChunk operation succeeded, verify that each mongos sees all data in the
            // chunk's range on only the toShard. If the operation failed, verify that each mongos
            // sees all data in the chunk's range on only the fromShard.
            var shardsForChunk =
                ChunkHelper.getShardsForRange(mongos, ns, chunk.min._id, chunk.max._id);
            var msg =
                msgBase + '\nMongos find().explain() results for chunk: ' + tojson(shardsForChunk);
            assertWhenOwnColl.eq(shardsForChunk.shards.length, 1, msg);
            if (moveChunkRes.ok) {
                msg = 'moveChunk succeeded but chunk was not on new shard.\n' + msg;
                assertWhenOwnColl.eq(shardsForChunk.shards[0], toShard, msg);
            } else {
                msg = 'moveChunk failed but chunk was not on original shard.\n' + msg;
                assertWhenOwnColl.eq(shardsForChunk.shards[0], fromShard, msg);
            }

            // If the moveChunk operation succeeded, verify that each mongos updated the chunk's
            // shard metadata with the toShard. If the operation failed, verify that each mongos
            // still sees the chunk's shard metadata as the fromShard.
            var chunkAfter = mongos.getDB('config').chunks.findOne({_id: chunk._id});
            var msg =
                msgBase + '\nchunkBefore: ' + tojson(chunk) + '\nchunkAfter: ' + tojson(chunkAfter);
            if (moveChunkRes.ok) {
                msg = "moveChunk succeeded but chunk's shard was not new shard.\n" + msg;
                assertWhenOwnColl.eq(chunkAfter.shard, toShard, msg);
            } else {
                msg = "moveChunk failed but chunk's shard was not original shard.\n" + msg;
                assertWhenOwnColl.eq(chunkAfter.shard, fromShard, msg);
            }
        }
    };

    $config.transitions = {init: {moveChunk: 1}, moveChunk: {moveChunk: 1}};

    return $config;
});
