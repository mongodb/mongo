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
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 * ]
 */
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/chunks.js";
import {isMongodConfigsvr} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

export const $config = (function () {
    let data = {
        partitionSize: 1,
        // We use a non-hashed shard key of { _id: 1 } so that documents reside on their expected
        // shard. The setup function creates documents with sequential numbering and gives
        // each shard its own numeric range to work with.
        shardKey: {_id: 1},
        shardKeyField: "_id",
    };

    data.makePartition = function makePartition(ns, tid, partitionSize) {
        let partition = {ns: ns};
        partition.lower = tid * partitionSize;
        partition.mid = tid * partitionSize + partitionSize / 2;
        partition.upper = tid * partitionSize + partitionSize;

        partition.isLowChunk = tid === 0 ? true : false;
        partition.isHighChunk = tid === this.threadCount - 1 ? true : false;

        partition.chunkLower = partition.isLowChunk ? MinKey : partition.lower;
        partition.chunkUpper = partition.isHighChunk ? MaxKey : partition.upper;

        // Unless only 1 thread, verify that we aren't both the high and low chunk.
        if (this.threadCount > 1) {
            assert(
                !(partition.isLowChunk && partition.isHighChunk),
                "should not be both the high and low chunk when there is more than 1 " + "thread:\n" + tojson(this),
            );
        } else {
            assert(
                partition.isLowChunk && partition.isHighChunk,
                "should be both the high and low chunk when there is only 1 thread:\n" + tojson(this),
            );
        }

        return partition;
    };

    // Intended for use on config servers only.
    // Get a random chunk within this thread's partition.
    data.getRandomChunkInPartition = function getRandomChunkInPartition(collName, conn) {
        assert(isMongodConfigsvr(conn.getDB("admin")), "Not connected to a mongod configsvr");
        assert(this.partition, "This function must be called from workloads that partition data across threads.");
        let coll = conn.getDB("config").chunks;
        // We must split up these cases because MinKey and MaxKey are not fully comparable.
        // This may be due to SERVER-18341, where the Matcher returns false positives in
        // comparison predicates with MinKey/MaxKey.
        const shardKeyField = this.shardKeyField[collName] || this.shardKeyField;
        let maxField = "max.";
        let minField = "min.";

        if (Array.isArray(shardKeyField)) {
            maxField += shardKeyField[0];
            minField += shardKeyField[0];
        } else {
            maxField += shardKeyField;
            minField += shardKeyField;
        }

        const chunksJoinClause = findChunksUtil.getChunksJoinClause(conn.getDB("config"), this.partition.ns);
        if (this.partition.isLowChunk && this.partition.isHighChunk) {
            return coll.aggregate([{$match: chunksJoinClause}, {$sample: {size: 1}}]).toArray()[0];
        } else if (this.partition.isLowChunk) {
            return coll
                .aggregate([
                    {
                        $match: Object.assign({[maxField]: {$lte: this.partition.chunkUpper}}, chunksJoinClause),
                    },
                    {$sample: {size: 1}},
                ])
                .toArray()[0];
        } else if (this.partition.isHighChunk) {
            return coll
                .aggregate([
                    {
                        $match: Object.assign({[minField]: {$gte: this.partition.chunkLower}}, chunksJoinClause),
                    },
                    {$sample: {size: 1}},
                ])
                .toArray()[0];
        } else {
            return coll
                .aggregate([
                    {
                        $match: Object.assign(
                            {
                                [minField]: {$gte: this.partition.chunkLower},
                                [maxField]: {$lte: this.partition.chunkUpper},
                            },
                            chunksJoinClause,
                        ),
                    },
                    {$sample: {size: 1}},
                ])
                .toArray()[0];
        }
    };

    // This is used by the extended workloads to perform additional setup for more splitPoints.
    data.setupAdditionalSplitPoints = function setupAdditionalSplitPoints(db, collName, partition) {};

    let states = (function () {
        // Inform this thread about its partition,
        // and verify that its partition is encapsulated in a single chunk.
        function init(db, collName, connCache) {
            let ns = db[collName].getFullName();

            // Inform this thread about its partition.
            // The tid of each thread is assumed to be in the range [0, this.threadCount).
            this.partition = this.makePartition(ns, this.tid, this.partitionSize);
            Object.freeze(this.partition);

            // Verify that there is exactly 1 chunk in our partition.
            let config = connCache.rsConns.config;
            let numChunks = ChunkHelper.getNumChunks(config, ns, this.partition.chunkLower, this.partition.chunkUpper);
            let chunks = ChunkHelper.getChunks(config, ns, MinKey, MaxKey);
            let msg = tojson({tid: this.tid, data: this.data, chunks: chunks});
            assert.eq(numChunks, 1, msg);
        }

        function dummy(db, collName, connCache) {}

        return {init: init, dummy: dummy};
    })();

    let transitions = {init: {dummy: 1}, dummy: {dummy: 1}};

    // Define each thread's data partition, populate it, and encapsulate it in a chunk.
    let setup = function setup(db, collName, cluster) {
        let ns = db[collName].getFullName();
        let configDB = db.getSiblingDB("config");

        // Sharding must be enabled on db[collName].
        let msg = "collection " + collName + " must be sharded.";
        assert.gte(findChunksUtil.findChunksByNs(configDB, ns).itcount(), 1, msg);

        for (let tid = 0; tid < this.threadCount; ++tid) {
            // Define this thread's partition.
            // The tid of each thread is assumed to be in the range [0, this.threadCount).
            let partition = this.makePartition(ns, tid, this.partitionSize);

            // Populate this thread's partition.
            let bulk = db[collName].initializeUnorderedBulkOp();
            for (let i = partition.lower; i < partition.upper; ++i) {
                bulk.insert({_id: i});
            }
            assert.commandWorked(bulk.execute());

            // Add split point for lower end of this thread's partition.
            // Since a split point will be created at the low end of each partition,
            // in the end each partition will be encompassed in its own chunk.
            // It's unnecessary to add a split point for the lower end for the thread
            // that has the lowest partition, because its chunk's lower end should be MinKey.
            if (!partition.isLowChunk) {
                assert.commandWorked(ChunkHelper.splitChunkAtPoint(db, collName, partition.lower));
            }

            this.setupAdditionalSplitPoints(db, collName, partition);
        }
    };

    let teardown = function teardown(db, collName, cluster) {};

    return {
        threadCount: 1,
        iterations: 1,
        startState: "init",
        states: states,
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
        passConnectionCache: true,
    };
})();
