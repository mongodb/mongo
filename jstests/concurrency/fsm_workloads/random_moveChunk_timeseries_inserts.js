/**
 * Tests the insertions into sharded time-series collection during a chunk migration. To ensure the
 * correctness, the test does the same inserts into an unsharded collection and verified that the
 * number of documents remain the same at the end. This test also checks that indexes on the
 * time-series buckets collection remain consistent after the test run.
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  requires_non_retryable_writes,
 *  does_not_support_transactions,
 *  requires_fcv_51,
 * ]
 */
'use strict';

load('jstests/concurrency/fsm_workload_helpers/chunks.js');  // for chunk helpers
load("jstests/core/timeseries/libs/timeseries.js");          // For 'TimeseriesTest' helpers.
load("jstests/libs/analyze_plan.js");                        // for 'getPlanStages'
load('jstests/concurrency/fsm_workloads/sharded_moveChunk_partitioned.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.data.nonShardCollName = "unsharded";

    // A random non-round start value was chosen so that we can verify the rounding behavior that
    // occurs while routing on mongos.
    $config.data.startTime = 1021;

    // One minute.
    $config.data.increment = 1000 * 60;

    // This should generate documents for a span of one month.
    $config.data.numInitialDocs = 60 * 24 * 30;
    $config.data.numMetaCount = 30;

    $config.data.featureFlagDisabled = true;

    $config.data.bucketPrefix = "system.buckets.";

    $config.data.timeField = 't';
    $config.data.metaField = 'm';

    $config.data.generateMetaFieldValueForInitialInserts = () => {
        return Math.floor(Random.rand() * $config.data.numMetaCount);
    };

    $config.data.generateMetaFieldValueForInsertStage = (i) => {
        return i % $config.data.numMetaCount;
    };

    $config.threadCount = 10;
    $config.iterations = 40;
    $config.startState = "init";

    $config.states.insert = function insert(db, collName, connCache) {
        if (this.featureFlagDisabled) {
            return;
        }

        for (let i = 0; i < 10; i++) {
            // Generate a random timestamp between 'startTime' and largest timestamp we inserted.
            const timer =
                this.startTime + Math.floor(Random.rand() * this.numInitialDocs * this.increment);
            const doc = {
                _id: new ObjectId(),
                [this.timeField]: new Date(timer),
                [this.metaField]: this.generateMetaFieldValueForInsertStage(this.tid),
            };
            assertAlways.commandWorked(db[collName].insert(doc));
            assertAlways.commandWorked(db[this.nonShardCollName].insert(doc));
        }
    };

    /**
     * Moves a random chunk in the target collection.
     */
    $config.states.moveChunk = function moveChunk(db, collName, connCache) {
        if (this.featureFlagDisabled) {
            return;
        }

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

    $config.states.init = function init(db, collName, connCache) {
        if (TimeseriesTest.shardedtimeseriesCollectionsEnabled(db.getMongo())) {
            this.featureFlagDisabled = false;
        }
    };

    $config.transitions = {
        init: {insert: 1},
        insert: {insert: 7, moveChunk: 1},
        moveChunk: {insert: 1, moveChunk: 0}
    };

    $config.teardown = function teardown(db, collName, cluster) {
        if (this.featureFlagDisabled) {
            return;
        }

        const numBuckets = db[this.bucketPrefix + collName].find({}).itcount();
        const numInitialDocs = db[collName].find().itcount();

        jsTestLog("NumBuckets " + numBuckets + ", numDocs on sharded cluster" +
                  db[collName].find().itcount() + "numDocs on unsharded collection " +
                  db[this.nonShardCollName].find({}).itcount());
        const pipeline =
            [{$project: {_id: "$_id", m: "$m", t: "$t"}}, {$sort: {m: 1, t: 1, _id: 1}}];
        const diff = DataConsistencyChecker.getDiff(db[collName].aggregate(pipeline),
                                                    db[this.nonShardCollName].aggregate(pipeline));
        assertAlways.eq(
            diff, {docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []});

        // Make sure that queries using various indexes on time-series buckets collection return
        // buckets with all documents.
        const verifyBucketIndex = (bucketIndex) => {
            const unpackStage = {
                "$_internalUnpackBucket": {
                    "timeField": this.timeField,
                    "metaField": this.metaField,
                    "bucketMaxSpanSeconds": NumberInt(3600)
                }
            };
            const bucketColl = db.getCollection(`system.buckets.${collName}`);
            const numDocsInBuckets =
                bucketColl.aggregate([{$sort: bucketIndex}, unpackStage]).itcount();
            assert.eq(numInitialDocs, numDocsInBuckets);
            const plan = bucketColl.explain().aggregate([{$sort: bucketIndex}]);
            const stages = getPlanStages(plan, 'IXSCAN');
            assert(stages.length > 0);
            for (let ixScan of stages) {
                assert.eq(bucketIndex, ixScan.keyPattern, ixScan);
            }
        };
        verifyBucketIndex({"control.min.t": 1});
        verifyBucketIndex({meta: 1, "control.min._id": 1, "control.max._id": 1});
        verifyBucketIndex({meta: 1, "control.min.t": 1, "control.max.t": 1});
    };

    $config.setup = function setup(db, collName, cluster) {
        if (TimeseriesTest.shardedtimeseriesCollectionsEnabled(db.getMongo())) {
            this.featureFlagDisabled = false;
        } else {
            return;
        }

        db[collName].drop();
        db[this.nonShardCollName].drop();

        assertAlways.commandWorked(db.createCollection(
            collName, {timeseries: {timeField: this.timeField, metaField: this.metaField}}));
        cluster.shardCollection(db[collName], {t: 1}, false);

        // Create indexes to verify index integrity during the teardown state.
        assert.commandWorked(db[this.nonShardCollName].createIndex({t: 1}));
        assert.commandWorked(db[collName].createIndex({m: 1, _id: 1}));
        assert.commandWorked(db[this.nonShardCollName].createIndex({m: 1, _id: 1}));
        assert.commandWorked(db[collName].createIndex({m: 1, t: 1}));
        assert.commandWorked(db[this.nonShardCollName].createIndex({m: 1, t: 1}));
        assert.commandWorked(db[this.nonShardCollName].createIndex({m: 1, t: 1, _id: 1}));

        const bulk = db[collName].initializeUnorderedBulkOp();
        const bulkUnsharded = db[this.nonShardCollName].initializeUnorderedBulkOp();

        let currentTimeStamp = this.startTime;
        for (let i = 0; i < this.numInitialDocs; ++i) {
            currentTimeStamp += this.increment;

            const doc = {
                _id: new ObjectId(),
                [this.timeField]: new Date(currentTimeStamp),
                [this.metaField]: this.generateMetaFieldValueForInitialInserts(i),
            };
            bulk.insert(doc);
            bulkUnsharded.insert(doc);
        }

        let res = bulk.execute();
        assertAlways.commandWorked(res);
        assertAlways.eq(this.numInitialDocs, res.nInserted);

        res = bulkUnsharded.execute();
        assertAlways.commandWorked(res);
        assertAlways.eq(this.numInitialDocs, res.nInserted);

        // Verify that the number of docs are same.
        assert.eq(db[collName].find().itcount(), db[this.nonShardCollName].find().itcount());

        // Pick 'this.threadCount - 1' split points so that we have can create this.threadCount
        // chunks.
        const chunkRange = (currentTimeStamp - this.startTime) / this.threadCount;
        currentTimeStamp = this.startTime;
        for (let i = 0; i < (this.threadCount - 1); ++i) {
            currentTimeStamp += chunkRange;
            assertWhenOwnColl.commandWorked(ChunkHelper.splitChunkAt(
                db, this.bucketPrefix + collName, {'control.min.t': new Date(currentTimeStamp)}));
        }

        // Verify that the number of docs remain the same.
        assert.eq(db[collName].find().itcount(), db[this.nonShardCollName].find().itcount());
    };

    return $config;
});
