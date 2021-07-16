'use strict';

/**
 * Extends sharded_mergeChunks_partitioned.js.
 *
 * Exercises concurrent moveChunk and mergeChunk operations, with each thread operating on its own
 * set of chunks.
 *
 * @tags: [requires_sharding, assumes_balancer_off, does_not_support_stepdowns]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');                       // for extendWorkload
load('jstests/concurrency/fsm_workloads/sharded_mergeChunks_partitioned.js');  // for $config
load("jstests/sharding/libs/find_chunks_util.js");

var $config = extendWorkload($config, function($config, $super) {
    $config.iterations = 6;
    $config.threadCount = 5;

    $config.data.partitionSize = 1100;  // number of shard key values
    $config.data.chunkSize = 1;
    $config.data.prevChunkSize = 64;
    $config.data.bigString = new Array(1024 * 128).join('x');

    $config.setup = function(db, collName, cluster) {
        this.isAutoSplitEnabled = cluster.isAutoSplitEnabled();
        if (this.isAutoSplitEnabled) {
            let configDB = cluster.getDB('config');
            configDB.settings.save({_id: "chunksize", value: /*<sizeInMB>*/ 1});
        }

        // Overridden methods should usually call the corresponding
        // method on $super.
        $super.setup.apply(this, arguments);
    };

    $config.teardown = function(db, collName, cluster) {
        if (cluster.isAutoSplitEnabled()) {
            jsTestLog('setting prevChunkSize: ' + this.prevChunkSize);
            cluster.getDB('config').settings.save({_id: "chunksize", value: this.prevChunkSize});
        }
        // $super.setup.teardown(this, arguments);
    };

    $config.data.setupAdditionalSplitPoints = function setupAdditionalSplitPoints(
        db, collName, partition) {
        let innerChunkSize = Math.floor(this.partitionSize / this.iterations);
        jsTestLog('innerChunkSize: ' + innerChunkSize);
        for (var i = 0; i < this.iterations; ++i) {
            var splitPoint = ((i + 1) * innerChunkSize) + partition.lower;
            assertAlways.commandWorked(ChunkHelper.splitChunkAtPoint(db, collName, splitPoint));
        }
    };

    // Split a random chunk in this thread's partition, and verify that each node
    // in the cluster affected by the splitChunk operation sees the appropriate
    // after-state regardless of whether the operation succeeded or failed.
    function doSplitChunk(db, collName, connCache, isAutoSplit) {
        var ns = db[collName].getFullName();
        var config = ChunkHelper.getPrimary(connCache.config);

        // Choose a random chunk in our partition to split.
        let chunk = this.getRandomChunkInPartition(collName, config);

        // Save the number of documents found in this chunk's range before the splitChunk
        // operation. This will be used to verify that the same number of documents in that
        // range are found after the splitChunk.
        // Choose the mongos randomly to distribute load.
        var numDocsBefore = ChunkHelper.getNumDocs(
            ChunkHelper.getRandomMongos(connCache.mongos), ns, chunk.min._id, chunk.max._id);

        // Save the number of chunks before the splitChunk operation. This will be used
        // to verify that the number of chunks after a successful splitChunk increases
        // by one, or after a failed splitChunk stays the same.
        var numChunksBefore = ChunkHelper.getNumChunks(
            config, ns, this.partition.chunkLower, this.partition.chunkUpper);

        assertAlways(typeof chunk.min._id, 'number');

        let splitChunkRes;
        if (isAutoSplit) {
            // assertAlways.eq(this.bigString.length, 1024 * 64);

            // try to insert
            let ret = db[collName].update(
                {_id: {$gte: chunk.min._id, $lt: chunk.max._id}, s: {$exists: false}},
                {$set: {s: this.bigString}});
            if (ret.modifiedCount == 0) {
                jsTestLog("Was not able to update any document in chunk " + tojson(chunk));
                splitChunkRes = {ok: false};
            } else {
                jsTestLog("Modified docs in chunk: " + tojson(chunk));

                let shardPrimary = ChunkHelper.getPrimary(connCache.shards[chunk.shard]);
                ret = shardPrimary.getDB("admin").runCommand({waitForOngoingChunkSplits: 1});
                if (!ret.hasOwnProperty("ok") && ret.ok) {
                    jsTestLog("Command waitForOngoingChunkSplits failed");
                }

                let numChunksAfter = ChunkHelper.getNumChunks(
                    config, ns, this.partition.chunkLower, this.partition.chunkUpper);
                splitChunkRes = {ok: numChunksAfter > numChunksBefore};
                jsTestLog("Autosplit happened: " + splitChunkRes.ok);
                print(db[collName].getShardDistribution());
            }

        } else if (chunk.min._id + 1 >= chunk.max._id || numDocsBefore <= 1) {
            jsTestLog("Chunk is not divisible " + tojson(chunk));
            splitChunkRes = {ok: false};
        } else {
            // Use chunk_helper.js's splitChunk wrapper to tolerate acceptable failures
            // and to use a limited number of retries with exponential backoff.
            var bounds = [{_id: chunk.min._id}, {_id: chunk.max._id}];
            splitChunkRes = ChunkHelper.splitChunkWithBounds(db, collName, bounds);
        }

        let msgBase = 'Result of splitChunk operation: ' + tojson(splitChunkRes);

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
            let res = conn.adminCommand({hello: 1});
            assertAlways.commandWorked(res);
            if (res.isWritablePrimary) {
                // If the splitChunk operation succeeded, verify that there are now
                // two chunks between the old chunk's lower and upper bounds.
                // If the operation failed, verify that there is still only one chunk
                // between the old chunk's lower and upper bounds.
                var numChunksBetweenOldChunksBounds =
                    ChunkHelper.getNumChunks(conn, ns, chunk.min._id, chunk.max._id);
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
                    config, ns, this.partition.chunkLower, this.partition.chunkUpper);
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

        // Verify that all mongos processes see the correct after-state on the shards and configs.
        // (see comments below for specifics).
        for (var mongos of connCache.mongos) {
            // Regardless of if the splitChunk operation succeeded or failed, verify that each
            // mongos sees as many documents in the chunk's range after the move as there were
            // before.
            var numDocsAfter = ChunkHelper.getNumDocs(mongos, ns, chunk.min._id, chunk.max._id);

            msg = 'Mongos does not see same number of documents after splitChunk.\n' + msgBase;
            assertWhenOwnColl.eq(numDocsAfter, numDocsBefore, msgBase);

            // Regardless of if the splitChunk operation succeeded or failed,
            // verify that each mongos sees all data in the original chunk's
            // range only on the shard the original chunk was on.
            var shardsForChunk =
                ChunkHelper.getShardsForRange(mongos, ns, chunk.min._id, chunk.max._id);
            msg = 'Mongos does not see exactly 1 shard for chunk after splitChunk.\n' + msgBase +
                '\n' +
                'Mongos find().explain() results for chunk: ' + tojson(shardsForChunk);
            assertWhenOwnColl.eq(shardsForChunk.shards.length, 1, msg);

            msg = 'Mongos sees different shard for chunk than chunk does after splitChunk.\n' +
                msgBase + '\n' +
                'Mongos find().explain() results for chunk: ' + tojson(shardsForChunk);
            assertWhenOwnColl.eq(shardsForChunk.shards[0], chunk.shard, msg);

            // If the splitChunk operation succeeded, verify that the mongos sees two chunks between
            // the old chunk's lower and upper bounds. If the operation failed, verify that the
            // mongos still only sees one chunk between the old chunk's lower and upper bounds.
            var numChunksBetweenOldChunksBounds =
                ChunkHelper.getNumChunks(mongos, ns, chunk.min._id, chunk.max._id);
            if (splitChunkRes.ok) {
                msg = 'splitChunk succeeded but the mongos does not see exactly 2 chunks ' +
                    'between the chunk bounds.\n' + msgBase;
                assertWhenOwnColl.eq(numChunksBetweenOldChunksBounds, 2, msg);
            } else {
                msg = 'splitChunk failed but the mongos does not see exactly 1 chunk between ' +
                    'the chunk bounds.\n' + msgBase;
                assertWhenOwnColl.eq(numChunksBetweenOldChunksBounds, 1, msg);
            }

            // If the splitChunk operation succeeded, verify that the total number of chunks in our
            // partition has increased by 1. If it failed, verify that it has stayed the same.
            var numChunksAfter = ChunkHelper.getNumChunks(
                mongos, ns, this.partition.chunkLower, this.partition.chunkUpper);
            if (splitChunkRes.ok) {
                msg = 'splitChunk succeeded but the mongos does nnot see exactly 1 more ' +
                    'chunk between the chunk bounds.\n' + msgBase;
                assertWhenOwnColl.eq(numChunksAfter, numChunksBefore + 1, msg);
            } else {
                msg = 'splitChunk failed but the mongos does not see the same number ' +
                    'of chunks between the chunk bounds.\n' + msgBase;
                assertWhenOwnColl.eq(numChunksAfter, numChunksBefore, msg);
            }
        }
    }

    $config.states.splitChunk = function(db, collName, connCache) {
        jsTestLog('Running splitChunk');
        doSplitChunk.apply(this, [db, collName, connCache, /*isAutoSplit*/ false]);
    };

    $config.states.splitChunkAuto = function(db, collName, connCache) {
        jsTestLog('Running splitChunkAuto');
        jsTestLog('isAutoSplitEnabled: ' + this.isAutoSplitEnabled);
        if (this.isAutoSplitEnabled) {
            doSplitChunk.apply(this, [db, collName, connCache, /*isAutoSplit*/ true]);
        }
    };

    $config.transitions = {
        init: {mergeChunks: 0.5, splitChunk: 0.3, splitChunkAuto: 0.2},
        mergeChunks: {mergeChunks: 0.5, splitChunk: 0.4, splitChunkAuto: 0.1},
        splitChunk: {mergeChunks: 0.5, splitChunk: 0.4, splitChunkAuto: 0.1},
        splitChunkAuto: {mergeChunks: 0.5, splitChunk: 0.5},
    };
    return $config;
});
