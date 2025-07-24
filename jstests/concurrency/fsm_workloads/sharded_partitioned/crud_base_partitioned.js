/**
 * Provides an init state that partitions the data space into logical ranges evenly across threads.
 *
 *      t1's data partition
 *       v
 *   (------------ | ------------ | ------------)  < t3's data partition
 *                      ^
 *                     t2's data partition
 *
 * Unlike sharded_base_partitioned, random_moveChunk_base, and sharded_moveChunk_partitioned this
 * does NOT require each parition to have its own chunk and should be preferred over the other bases
 * when chunk separation is not strictly necessary and partitions are only needed to prevent
 * non-chunk operations from conflicting.
 *
 * Because there is no pre-splitting, this base is suitable to be used with the balancer enabled
 * without any special setup required.
 *
 * @tags: [
 *  requires_sharding,
 * ]
 */

export const $config = (function() {
    var data = {
        partitionSize: 1,
        // We use a non-hashed shard key of { skey: 1 } so that documents reside on their expected
        // shard. The setup function creates documents with sequential numbering and gives
        // each shard its own numeric range to work with.
        shardKey: {skey: 1},
        shardKeyField: {},
        defaultShardKeyField: 'skey',
    };

    data.makePartition = function makePartition(ns, tid, partitionSize) {
        var partition = {ns: ns};
        partition.lower = tid * partitionSize;
        partition.mid = (tid * partitionSize) + (partitionSize / 2);
        partition.upper = (tid * partitionSize) + partitionSize;

        partition.isLowChunk = (tid === 0) ? true : false;
        partition.isHighChunk = (tid === (this.threadCount - 1)) ? true : false;

        partition.chunkLower = partition.isLowChunk ? MinKey : partition.lower;
        partition.chunkUpper = partition.isHighChunk ? MaxKey : partition.upper;

        // Unless only 1 thread, verify that we aren't both the high and low chunk.
        if (this.threadCount > 1) {
            assert(!(partition.isLowChunk && partition.isHighChunk),
                   'should not be both the high and low chunk when there is more than 1 ' +
                       'thread:\n' + tojson(this));
        } else {
            assert(partition.isLowChunk && partition.isHighChunk,
                   'should be both the high and low chunk when there is only 1 thread:\n' +
                       tojson(this));
        }

        return partition;
    };

    var states = (function() {
        // Inform this thread about its partition
        function init(db, collName, connCache) {
            var ns = db[collName].getFullName();

            // Inform this thread about its partition.
            // The tid of each thread is assumed to be in the range [0, this.threadCount).
            this.partition = this.makePartition(ns, this.tid, this.partitionSize);
            Object.freeze(this.partition);
        }

        function dummy(db, collName, connCache) {
        }

        return {init: init, dummy: dummy};
    })();

    var transitions = {init: {dummy: 1}, dummy: {dummy: 1}};

    // Define each thread's data partition, populate it, and encapsulate it in a chunk.
    var setup = function setup(db, collName, cluster) {
        var ns = db[collName].getFullName();
        for (var tid = 0; tid < this.threadCount; ++tid) {
            // Define this thread's partition.
            // The tid of each thread is assumed to be in the range [0, this.threadCount).
            var partition = this.makePartition(ns, tid, this.partitionSize);

            // Populate this thread's partition.
            var bulk = db[collName].initializeUnorderedBulkOp();
            for (var i = partition.lower; i < partition.upper; ++i) {
                bulk.insert({skey: i});
            }
            assert.commandWorked(bulk.execute());
        }
    };

    var teardown = function teardown(db, collName, cluster) {};

    return {
        threadCount: 1,
        iterations: 1,
        startState: 'init',
        states: states,
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
        passConnectionCache: true
    };
})();
