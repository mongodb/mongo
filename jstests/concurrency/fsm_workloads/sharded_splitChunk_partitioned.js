'use strict';

/**
 * Extends sharded_base_partitioned.js.
 *
 * Exercises the concurrent splitChunk operations, but each thread operates on its own set of
 * chunks.
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');                // for extendWorkload
load('jstests/concurrency/fsm_workloads/sharded_base_partitioned.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.iterations = 5;
    $config.threadCount = 5;

    $config.data.partitionSize = 100;  // number of shard key values

    // Split a random chunk in this thread's partition, and verify that each node
    // in the cluster affected by the splitChunk operation sees the appropriate
    // after-state regardless of whether the operation succeeded or failed.
    $config.states.splitChunk = function splitChunk(db, collName, connCache) {

        var dbName = db.getName();
        var ns = db[collName].getFullName();
        var config = ChunkHelper.getPrimary(connCache.config);

        // Choose a random chunk in our partition to split.
        var chunk = this.getRandomChunkInPartition(config);

        // Save the number of documents found in this chunk's range before the splitChunk
        // operation. This will be used to verify that the same number of documents in that
        // range are found after the splitChunk.
        // Choose the bongos randomly to distribute load.
        var numDocsBefore = ChunkHelper.getNumDocs(
            ChunkHelper.getRandomBongos(connCache.bongos), ns, chunk.min._id, chunk.max._id);

        // Save the number of chunks before the splitChunk operation. This will be used
        // to verify that the number of chunks after a successful splitChunk increases
        // by one, or after a failed splitChunk stays the same.
        var numChunksBefore =
            ChunkHelper.getNumChunks(config, this.partition.chunkLower, this.partition.chunkUpper);

        // Use chunk_helper.js's splitChunk wrapper to tolerate acceptable failures
        // and to use a limited number of retries with exponential backoff.
        var bounds = [{_id: chunk.min._id}, {_id: chunk.max._id}];
        var splitChunkRes = ChunkHelper.splitChunkWithBounds(db, collName, bounds);
        var msgBase = 'Result of splitChunk operation: ' + tojson(splitChunkRes);

        // Regardless of whether the splitChunk operation succeeded or failed,
        // verify that the shard the original chunk was on returns all data for the chunk.
        var shardPrimary = ChunkHelper.getPrimary(connCache.shards[chunk.shard]);
        var shardNumDocsAfter =
            ChunkHelper.getNumDocs(shardPrimary, ns, chunk.min._id, chunk.max._id);
        var msg = 'Shard does not have same number of documents after splitChunk.\n' + msgBase;
        assertWhenOwnColl.eq(shardNumDocsAfter, numDocsBefore, msg);

        // Verify that all config servers have the correct after-state.
        // (see comments below for specifics).
        for (var conn of connCache.config) {
            var res = conn.adminCommand({isMaster: 1});
            assertAlways.commandWorked(res);
            if (res.ismaster) {
                // If the splitChunk operation succeeded, verify that there are now
                // two chunks between the old chunk's lower and upper bounds.
                // If the operation failed, verify that there is still only one chunk
                // between the old chunk's lower and upper bounds.
                var numChunksBetweenOldChunksBounds =
                    ChunkHelper.getNumChunks(conn, chunk.min._id, chunk.max._id);
                if (splitChunkRes.ok) {
                    msg = 'splitChunk succeeded but the config does not see exactly 2 chunks ' +
                        'between the chunk bounds.\n' + msgBase;
                    assertWhenOwnColl.eq(numChunksBetweenOldChunksBounds, 2, msg);
                } else {
                    msg = 'splitChunk failed but the config does not see exactly 1 chunk between ' +
                        'the chunk bounds.\n' + msgBase;
                    assertWhenOwnColl.eq(numChunksBetweenOldChunksBounds, 1, msg);
                }

                // If the splitChunk operation succeeded, verify that the total number
                // of chunks in our partition has increased by 1. If it failed, verify
                // that it has stayed the same.
                var numChunksAfter = ChunkHelper.getNumChunks(
                    config, this.partition.chunkLower, this.partition.chunkUpper);
                if (splitChunkRes.ok) {
                    msg = 'splitChunk succeeded but the config does nnot see exactly 1 more ' +
                        'chunk between the chunk bounds.\n' + msgBase;
                    assertWhenOwnColl.eq(numChunksAfter, numChunksBefore + 1, msg);
                } else {
                    msg = 'splitChunk failed but the config does not see the same number ' +
                        'of chunks between the chunk bounds.\n' + msgBase;
                    assertWhenOwnColl.eq(numChunksAfter, numChunksBefore, msg);
                }
            }
        }

        // Verify that all bongos processes see the correct after-state on the shards and configs.
        // (see comments below for specifics).
        for (var bongos of connCache.bongos) {
            // Regardless of if the splitChunk operation succeeded or failed, verify that each
            // bongos sees as many documents in the chunk's range after the move as there were
            // before.
            var numDocsAfter = ChunkHelper.getNumDocs(bongos, ns, chunk.min._id, chunk.max._id);

            msg = 'Bongos does not see same number of documents after splitChunk.\n' + msgBase;
            assertWhenOwnColl.eq(numDocsAfter, numDocsBefore, msgBase);

            // Regardless of if the splitChunk operation succeeded or failed,
            // verify that each bongos sees all data in the original chunk's
            // range only on the shard the original chunk was on.
            var shardsForChunk =
                ChunkHelper.getShardsForRange(bongos, ns, chunk.min._id, chunk.max._id);
            msg = 'Bongos does not see exactly 1 shard for chunk after splitChunk.\n' + msgBase +
                '\n' +
                'Bongos find().explain() results for chunk: ' + tojson(shardsForChunk);
            assertWhenOwnColl.eq(shardsForChunk.shards.length, 1, msg);

            msg = 'Bongos sees different shard for chunk than chunk does after splitChunk.\n' +
                msgBase + '\n' +
                'Bongos find().explain() results for chunk: ' + tojson(shardsForChunk);
            assertWhenOwnColl.eq(shardsForChunk.shards[0], chunk.shard, msg);

            // If the splitChunk operation succeeded, verify that the bongos sees two chunks between
            // the old chunk's lower and upper bounds. If the operation failed, verify that the
            // bongos still only sees one chunk between the old chunk's lower and upper bounds.
            var numChunksBetweenOldChunksBounds =
                ChunkHelper.getNumChunks(bongos, chunk.min._id, chunk.max._id);
            if (splitChunkRes.ok) {
                msg = 'splitChunk succeeded but the bongos does not see exactly 2 chunks ' +
                    'between the chunk bounds.\n' + msgBase;
                assertWhenOwnColl.eq(numChunksBetweenOldChunksBounds, 2, msg);
            } else {
                msg = 'splitChunk failed but the bongos does not see exactly 1 chunk between ' +
                    'the chunk bounds.\n' + msgBase;
                assertWhenOwnColl.eq(numChunksBetweenOldChunksBounds, 1, msg);
            }

            // If the splitChunk operation succeeded, verify that the total number of chunks in our
            // partition has increased by 1. If it failed, verify that it has stayed the same.
            var numChunksAfter = ChunkHelper.getNumChunks(
                bongos, this.partition.chunkLower, this.partition.chunkUpper);
            if (splitChunkRes.ok) {
                msg = 'splitChunk succeeded but the bongos does nnot see exactly 1 more ' +
                    'chunk between the chunk bounds.\n' + msgBase;
                assertWhenOwnColl.eq(numChunksAfter, numChunksBefore + 1, msg);
            } else {
                msg = 'splitChunk failed but the bongos does not see the same number ' +
                    'of chunks between the chunk bounds.\n' + msgBase;
                assertWhenOwnColl.eq(numChunksAfter, numChunksBefore, msg);
            }
        }
    };

    $config.transitions = {init: {splitChunk: 1}, splitChunk: {splitChunk: 1}};

    return $config;
});
