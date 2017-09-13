'use strict';

/**
 * Provides an init state that partitions the data space into chunks evenly across threads.
 *
 *      t1's data partition encapsulated in own chunk
 *       v
 *   ------------) | [------------) | [------------  < t3's data partition in own chunk
 *                      ^
 *                     t2's data partition encapsulated in own chunk
 *
 * Intended to allow mergeChunks, moveChunk, and splitChunk operations to stay
 * within the bounds of a thread's partition.
 *
 *   <==t1's partition==>                           <==t3's partition==>
 *
 *   ---)[--)[----)[---) | [---)[---)[----)[-)[) | [-------)[-)[--------
 *
 *                         <===t2's partition==>
 */

load('jstests/concurrency/fsm_workload_helpers/chunks.js');  // for chunk helpers

var $config = (function() {

    var data = {
        partitionSize: 1,
        // We use a non-hashed shard key of { _id: 1 } so that documents reside on their expected
        // shard. The setup function creates documents with sequential numbering and gives
        // each shard its own numeric range to work with.
        shardKey: {_id: 1},
    };

    data.makePartition = function makePartition(tid, partitionSize) {
        var partition = {};
        partition.lower = tid * partitionSize;
        partition.upper = (tid * partitionSize) + partitionSize;

        partition.isLowChunk = (tid === 0) ? true : false;
        partition.isHighChunk = (tid === (this.threadCount - 1)) ? true : false;

        partition.chunkLower = partition.isLowChunk ? MinKey : partition.lower;
        partition.chunkUpper = partition.isHighChunk ? MaxKey : partition.upper;

        // Unless only 1 thread, verify that we aren't both the high and low chunk.
        if (this.threadCount > 1) {
            assertAlways(!(partition.isLowChunk && partition.isHighChunk),
                         'should not be both the high and low chunk when there is more than 1 ' +
                             'thread:\n' + tojson(this));
        } else {
            assertAlways(partition.isLowChunk && partition.isHighChunk,
                         'should be both the high and low chunk when there is only 1 thread:\n' +
                             tojson(this));
        }

        return partition;
    };

    // Intended for use on config servers only.
    // Get a random chunk within this thread's partition.
    data.getRandomChunkInPartition = function getRandomChunkInPartition(conn) {
        assert(isMongodConfigsvr(conn.getDB('admin')), 'Not connected to a mongod configsvr');
        assert(this.partition,
               'This function must be called from workloads that partition data across threads.');
        var coll = conn.getDB('config').chunks;
        // We must split up these cases because MinKey and MaxKey are not fully comparable.
        // This may be due to SERVER-18341, where the Matcher returns false positives in
        // comparison predicates with MinKey/MaxKey.
        if (this.partition.isLowChunk && this.partition.isHighChunk) {
            return coll.aggregate([{$sample: {size: 1}}]).toArray()[0];
        } else if (this.partition.isLowChunk) {
            return coll
                .aggregate([
                    {$match: {'max._id': {$lte: this.partition.chunkUpper}}},
                    {$sample: {size: 1}}
                ])
                .toArray()[0];
        } else if (this.partition.isHighChunk) {
            return coll
                .aggregate([
                    {$match: {'min._id': {$gte: this.partition.chunkLower}}},
                    {$sample: {size: 1}}
                ])
                .toArray()[0];
        } else {
            return coll
                .aggregate([
                    {
                      $match: {
                          'min._id': {$gte: this.partition.chunkLower},
                          'max._id': {$lte: this.partition.chunkUpper}
                      }
                    },
                    {$sample: {size: 1}}
                ])
                .toArray()[0];
        }
    };

    // This is used by the extended workloads to perform additional setup for more splitPoints.
    data.setupAdditionalSplitPoints = function setupAdditionalSplitPoints(db, collName, partition) {
    };

    var states = (function() {
        // Inform this thread about its partition,
        // and verify that its partition is encapsulated in a single chunk.
        function init(db, collName, connCache) {
            // Inform this thread about its partition.
            // The tid of each thread is assumed to be in the range [0, this.threadCount).
            this.partition = this.makePartition(this.tid, this.partitionSize);
            Object.freeze(this.partition);

            // Verify that there is exactly 1 chunk in our partition.
            var config = ChunkHelper.getPrimary(connCache.config);
            var numChunks = ChunkHelper.getNumChunks(
                config, this.partition.chunkLower, this.partition.chunkUpper);
            var chunks = ChunkHelper.getChunks(config, MinKey, MaxKey);
            var msg = tojson({tid: this.tid, data: this.data, chunks: chunks});
            assertWhenOwnColl.eq(numChunks, 1, msg);
        }

        function dummy(db, collName, connCache) {
        }

        return {init: init, dummy: dummy};
    })();

    var transitions = {init: {dummy: 1}, dummy: {dummy: 1}};

    // Define each thread's data partition, populate it, and encapsulate it in a chunk.
    var setup = function setup(db, collName, cluster) {
        var dbName = db.getName();
        var ns = db[collName].getFullName();
        var configDB = db.getSiblingDB('config');

        // Sharding must be enabled on db.
        var res = configDB.databases.findOne({_id: dbName});
        var msg = 'db ' + dbName + ' must be sharded.';
        assertAlways(res.partitioned, msg);

        // Sharding must be enabled on db[collName].
        msg = 'collection ' + collName + ' must be sharded.';
        assertAlways.gte(configDB.chunks.find({ns: ns}).itcount(), 1, msg);

        for (var tid = 0; tid < this.threadCount; ++tid) {
            // Define this thread's partition.
            // The tid of each thread is assumed to be in the range [0, this.threadCount).
            var partition = this.makePartition(tid, this.partitionSize);

            // Populate this thread's partition.
            var bulk = db[collName].initializeUnorderedBulkOp();
            for (var i = partition.lower; i < partition.upper; ++i) {
                bulk.insert({_id: i});
            }
            assertAlways.writeOK(bulk.execute());

            // Add split point for lower end of this thread's partition.
            // Since a split point will be created at the low end of each partition,
            // in the end each partition will be encompassed in its own chunk.
            // It's unnecessary to add a split point for the lower end for the thread
            // that has the lowest partition, because its chunk's lower end should be MinKey.
            if (!partition.isLowChunk) {
                assertWhenOwnColl.commandWorked(
                    ChunkHelper.splitChunkAtPoint(db, collName, partition.lower));
            }

            this.setupAdditionalSplitPoints(db, collName, partition);
        }

    };

    var skip = function skip(cluster) {
        if (!cluster.isSharded() || cluster.isAutoSplitEnabled() || cluster.isBalancerEnabled()) {
            return {
                skip: true,
                msg: 'only runs in a sharded cluster with autoSplit & balancer disabled.'
            };
        }
        return {skip: false};
    };

    return {
        threadCount: 1,
        iterations: 1,
        startState: 'init',
        states: states,
        transitions: transitions,
        data: data,
        setup: setup,
        skip: skip,
        passConnectionCache: true
    };
})();
