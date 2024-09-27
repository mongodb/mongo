/**
 * Tests insertMany into a time-series collection during a chunk migration. This test is not
 * checking results but is meant to run in sanitizers to ensure the exception handling in the
 * time-series insert many code is correct.
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  does_not_support_transactions,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/chunks.js";
import {
    $config as $baseConfig
} from 'jstests/concurrency/fsm_workloads/sharded_partitioned/sharded_moveChunk_partitioned.js';
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    // A random non-round start value was chosen so that we can verify the rounding behavior that
    // occurs while routing on mongos.
    $config.data.startTime = 1021;

    // One minute.
    $config.data.increment = 1000 * 60;

    // This should generate documents for a span of one month.
    $config.data.numInitialDocs = 60 * 24 * 30;

    $config.data.bucketPrefix = "system.buckets.";

    $config.data.metaField = 'm';
    $config.data.timeField = 't';

    $config.threadCount = 10;
    $config.iterations = 40;
    $config.startState = "init";

    /**
     * Perform insertMany with ordered:false that may target multiple buckets across multiple chunks
     */
    $config.states.insert = function insert(db, collName, connCache) {
        var docs = [];
        for (let i = 0; i < 10; i++) {
            // Generate a random timestamp between 'startTime' and largest timestamp we inserted.
            const timer =
                this.startTime + Math.floor(Random.rand() * this.numInitialDocs * this.increment);
            const doc = {
                _id: new ObjectId(),
                [this.metaField]: 0,
                [this.timeField]: new Date(timer),
            };
            docs.push(doc);
        }

        // Perform unordered insertMany. When this is done concurrent with chunk migrations we may
        // get an exception in mongod when only a subset of documents have been (partially)
        // processed. This will be retried by mongos and no error is observed for the user. This
        // test is meant to be run with sanitizers to ensure correctness.
        assert.commandWorked(db[collName].insertMany(docs, {ordered: false}));
    };

    /**
     * Moves a random chunk in the target collection.
     */
    $config.states.moveChunk = function moveChunk(db, collName, connCache) {
        const configDB = db.getSiblingDB('config');
        const ns = db[this.bucketPrefix + collName].getFullName();
        const chunks = findChunksUtil.findChunksByNs(configDB, ns).toArray();
        const chunkToMove = chunks[this.tid];
        const fromShard = chunkToMove.shard;

        // Choose a random shard to move the chunk to.
        const shardNames = Object.keys(connCache.shards);
        const destinationShards = shardNames.filter(function(shard) {
            if (shard !== fromShard) {
                return shard;
            }
        });
        const toShard = destinationShards[Random.randInt(destinationShards.length)];
        const waitForDelete = false;
        ChunkHelper.moveChunk(db,
                              this.bucketPrefix + collName,
                              [chunkToMove.min, chunkToMove.max],
                              toShard,
                              waitForDelete);
    };

    $config.states.init = function init(db, collName, connCache) {};

    $config.transitions = {
        init: {insert: 1},
        insert: {insert: 7, moveChunk: 1},
        moveChunk: {insert: 1, moveChunk: 0}
    };

    $config.setup = function setup(db, collName, cluster) {
        db[collName].drop();

        assert.commandWorked(db.createCollection(
            collName, {timeseries: {metaField: this.metaField, timeField: this.timeField}}));
        cluster.shardCollection(db[collName], {t: 1}, false);

        const bulk = db[collName].initializeUnorderedBulkOp();

        let currentTimeStamp = this.startTime;
        for (let i = 0; i < this.numInitialDocs; ++i) {
            currentTimeStamp += this.increment;

            const metaVal = 0;
            const doc = {
                _id: new ObjectId(),
                [this.metaField]: metaVal,
                [this.timeField]: new Date(currentTimeStamp),
                // use an invalid tid to not count these documents when we validate at teardown
                tid: -1,
            };
            bulk.insert(doc);
        }

        let res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(this.numInitialDocs, res.nInserted);

        // Pick 'this.threadCount - 1' split points so that we have 'this.threadCount' chunks.
        const chunkRange = (currentTimeStamp - this.startTime) / this.threadCount;
        currentTimeStamp = this.startTime;
        for (let i = 0; i < (this.threadCount - 1); ++i) {
            currentTimeStamp += chunkRange;
            assert.commandWorked(ChunkHelper.splitChunkAt(
                db, this.bucketPrefix + collName, {'control.min.t': new Date(currentTimeStamp)}));
        }

        // Create an extra chunk on each shard to make sure multi:true operations return correct
        // metrics in write results.
        const destinationShards = Object.keys(cluster.getSerializedCluster().shards);
        for (const destinationShard of destinationShards) {
            currentTimeStamp += chunkRange;
            assert.commandWorked(ChunkHelper.splitChunkAt(
                db, this.bucketPrefix + collName, {'control.min.t': new Date(currentTimeStamp)}));

            ChunkHelper.moveChunk(
                db,
                this.bucketPrefix + collName,
                [{'control.min.t': new Date(currentTimeStamp)}, {'control.min.t': MaxKey}],
                destinationShard,
                /*waitForDelete=*/ false);
        }
    };

    return $config;
});
