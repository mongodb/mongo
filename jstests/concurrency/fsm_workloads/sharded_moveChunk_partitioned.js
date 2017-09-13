'use strict';

/**
 * Extends sharded_base_partitioned.js.
 *
 * Exercises the concurrent moveChunk operations, but each thread operates on its own set of
 * chunks.
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
        var dbName = db.getName();
        var ns = db[collName].getFullName();
        var config = ChunkHelper.getPrimary(connCache.config);

        // Verify that more than one shard exists in the cluster. If only one shard existed,
        // there would be no way to move a chunk from one shard to another.
        var numShards = config.getDB('config').shards.find().itcount();
        var msg = 'There must be more than one shard when performing a moveChunks operation\n' +
            'shards: ' + tojson(config.getDB('config').shards.find().toArray());
        assertAlways.gt(numShards, 1, msg);

        // Choose a random chunk in our partition to move.
        var chunk = this.getRandomChunkInPartition(config);
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
        var numChunksBefore =
            ChunkHelper.getNumChunks(config, this.partition.chunkLower, this.partition.chunkUpper);

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
        var fromShardPrimary = ChunkHelper.getPrimary(connCache.shards[fromShard]);
        var toShardPrimary = ChunkHelper.getPrimary(connCache.shards[toShard]);
        var fromShardNumDocsAfter =
            ChunkHelper.getNumDocs(fromShardPrimary, ns, chunk.min._id, chunk.max._id);
        var toShardNumDocsAfter =
            ChunkHelper.getNumDocs(toShardPrimary, ns, chunk.min._id, chunk.max._id);
        // If the moveChunk operation succeeded, verify that the shard the chunk
        // was moved to returns all data for the chunk. If waitForDelete was true,
        // also verify that the shard the chunk was moved from returns no data for the chunk.
        if (moveChunkRes.ok) {
            if (waitForDelete) {
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
            msg = 'moveChunk failed but new shard had documents.\n' + msgBase +
                ', waitForDelete: ' + waitForDelete + ', bounds: ' + tojson(bounds);
            assertWhenOwnColl.eq(toShardNumDocsAfter, 0, msg);
        }

        // Verify that all config servers have the correct after-state.
        // (see comments below for specifics).
        for (var conn of connCache.config) {
            var res = conn.adminCommand({isMaster: 1});
            assertAlways.commandWorked(res);
            if (res.ismaster) {
                // If the moveChunk operation succeeded, verify that the config updated the chunk's
                // shard with the toShard. If the operation failed, verify that the config kept
                // the chunk's shard as the fromShard.
                var chunkAfter = conn.getDB('config').chunks.findOne({_id: chunk._id});
                var msg = msgBase + '\nchunkBefore: ' + tojson(chunk) + '\nchunkAfter: ' +
                    tojson(chunkAfter);
                if (moveChunkRes.ok) {
                    msg = "moveChunk succeeded but chunk's shard was not new shard.\n" + msg;
                    assertWhenOwnColl.eq(chunkAfter.shard, toShard, msg);
                } else {
                    msg = "moveChunk failed but chunk's shard was not original shard.\n" + msg;
                    assertWhenOwnColl.eq(chunkAfter.shard, fromShard, msg);
                }

                // Regardless of whether the operation succeeded or failed,
                // verify that the number of chunks in our partition stayed the same.
                var numChunksAfter = ChunkHelper.getNumChunks(
                    conn, this.partition.chunkLower, this.partition.chunkUpper);
                msg = 'Number of chunks in partition seen by config changed with moveChunk.\n' +
                    msgBase;
                assertWhenOwnColl.eq(numChunksBefore, numChunksAfter, msg);
            }
        }

        // Verify that all mongos processes see the correct after-state on the shards and configs.
        // (see comments below for specifics).
        for (var mongos of connCache.mongos) {
            // Regardless of if the moveChunk operation succeeded or failed,
            // verify that each mongos sees as many documents in the chunk's
            // range after the move as there were before.
            var numDocsAfter = ChunkHelper.getNumDocs(mongos, ns, chunk.min._id, chunk.max._id);
            msg =
                'Number of chunks in partition seen by mongos changed with moveChunk.\n' + msgBase;
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
