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
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/chunks.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/sharded_partitioned/sharded_moveChunk_partitioned.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getPlanStages} from "jstests/libs/query/analyze_plan.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // This test manually shards the collection.
    TestData.shardCollectionProbability = 0;

    $config.data.nonShardCollName = "unsharded";

    // A random non-round start value was chosen so that we can verify the rounding behavior that
    // occurs while routing on mongos.
    $config.data.startTime = 1021;

    // One minute.
    $config.data.increment = 1000 * 60;

    // This should generate documents for a span of one month.
    $config.data.numInitialDocs = 60 * 24 * 30;
    $config.data.numMetaCount = 30;

    $config.data.metaField = "m";
    $config.data.timeField = "t";

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
        for (let i = 0; i < 10; i++) {
            // Generate a random timestamp between 'startTime' and largest timestamp we inserted.
            const timer = this.startTime + Math.floor(Random.rand() * this.numInitialDocs * this.increment);
            const metaVal = this.generateMetaFieldValueForInsertStage(this.tid);
            const doc = {
                _id: new ObjectId(),
                [this.metaField]: metaVal,
                [this.timeField]: new Date(timer),
                f: metaVal,
            };
            TimeseriesTest.assertInsertWorked(db[collName].insert(doc));
            TimeseriesTest.assertInsertWorked(db[this.nonShardCollName].insert(doc));
        }
    };

    /**
     * Moves a random chunk in the target collection.
     */
    $config.states.moveChunk = function moveChunk(db, collName, connCache) {
        const configDB = db.getSiblingDB("config");
        const coll = getTimeseriesCollForDDLOps(db, db[collName]);
        const chunks = findChunksUtil.findChunksByNs(configDB, coll.getFullName()).toArray();
        const chunkToMove = chunks[this.tid];
        const fromShard = chunkToMove.shard;

        // Choose a random shard to move the chunk to.
        const shardNames = Object.keys(connCache.shards);
        const destinationShards = shardNames.filter(function (shard) {
            if (shard !== fromShard) {
                return shard;
            }
        });
        const toShard = destinationShards[Random.randInt(destinationShards.length)];
        const waitForDelete = false;
        ChunkHelper.moveChunk(db, coll.getName(), [chunkToMove.min, chunkToMove.max], toShard, waitForDelete);
    };

    $config.states.init = function init(db, collName, connCache) {};

    $config.transitions = {
        init: {insert: 1},
        insert: {insert: 7, moveChunk: 1},
        moveChunk: {insert: 1, moveChunk: 0},
    };

    $config.data.validateCollection = function validate(db, collName) {
        const pipeline = [{$project: {_id: "$_id", m: "$m", t: "$t"}}, {$sort: {m: 1, t: 1, _id: 1}}];
        const diff = DataConsistencyChecker.getDiff(
            db[collName].aggregate(pipeline),
            db[this.nonShardCollName].aggregate(pipeline),
        );
        assert.eq(diff, {docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []});
    };

    $config.teardown = function teardown(db, collName, cluster) {
        const numBuckets = getTimeseriesCollForRawOps(db, db[collName]).find({}).rawData().itcount();
        const numInitialDocs = db[collName].find().itcount();

        jsTestLog(
            "NumBuckets " +
                numBuckets +
                ", numDocs on sharded cluster" +
                db[collName].find().itcount() +
                "numDocs on unsharded collection " +
                db[this.nonShardCollName].find({}).itcount(),
        );

        // Validate the contents of the collection.
        this.validateCollection(db, collName);

        // Make sure that queries using various indexes on time-series buckets collection return
        // buckets with all documents.
        const verifyBucketIndex = (bucketIndex) => {
            const unpackStage = {
                "$_internalUnpackBucket": {
                    "metaField": this.metaField,
                    "timeField": this.timeField,
                    "bucketMaxSpanSeconds": NumberInt(3600),
                },
            };
            const numDocsInBuckets = getTimeseriesCollForRawOps(db, db[collName])
                .aggregate([{$sort: bucketIndex}, unpackStage], getRawOperationSpec(db))
                .itcount();
            assert.eq(numInitialDocs, numDocsInBuckets);
            const plan = getTimeseriesCollForRawOps(db, db[collName])
                .explain()
                .aggregate([{$sort: bucketIndex}], getRawOperationSpec(db));
            const stages = getPlanStages(plan, "IXSCAN");
            assert(stages.length > 0);
            for (let ixScan of stages) {
                assert.eq(bucketIndex, ixScan.keyPattern, ixScan);
            }
        };

        verifyBucketIndex({"control.min.t": 1, "control.max.t": 1});
        verifyBucketIndex({meta: 1, "control.min._id": 1, "control.max._id": 1});
        verifyBucketIndex({meta: 1, "control.min.t": 1, "control.max.t": 1});
    };

    $config.setup = function setup(db, collName, cluster) {
        db[collName].drop();
        db[this.nonShardCollName].drop();

        assert.commandWorked(
            db.createCollection(collName, {timeseries: {metaField: this.metaField, timeField: this.timeField}}),
        );
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

            const metaVal = this.generateMetaFieldValueForInitialInserts(i);
            const doc = {
                _id: new ObjectId(),
                [this.metaField]: metaVal,
                [this.timeField]: new Date(currentTimeStamp),
                f: metaVal,
            };
            bulk.insert(doc);
            bulkUnsharded.insert(doc);
        }

        let res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(this.numInitialDocs, res.nInserted);

        res = bulkUnsharded.execute();
        assert.commandWorked(res);
        assert.eq(this.numInitialDocs, res.nInserted);

        // Verify that the number of docs are same.
        assert.eq(db[collName].find().itcount(), db[this.nonShardCollName].find().itcount());

        // Pick 'this.threadCount - 1' split points so that we have 'this.threadCount' chunks.
        const chunkRange = (currentTimeStamp - this.startTime) / this.threadCount;
        currentTimeStamp = this.startTime;
        for (let i = 0; i < this.threadCount - 1; ++i) {
            currentTimeStamp += chunkRange;
            assert.commandWorked(
                ChunkHelper.splitChunkAt(db, getTimeseriesCollForDDLOps(db, db[collName]).getName(), {
                    "control.min.t": new Date(currentTimeStamp),
                }),
            );
        }

        // Create an extra chunk on each shard to make sure multi:true operations return correct
        // metrics in write results.
        const destinationShards = Object.keys(cluster.getSerializedCluster().shards);
        for (const destinationShard of destinationShards) {
            currentTimeStamp += chunkRange;
            assert.commandWorked(
                ChunkHelper.splitChunkAt(db, getTimeseriesCollForDDLOps(db, db[collName]).getName(), {
                    "control.min.t": new Date(currentTimeStamp),
                }),
            );

            ChunkHelper.moveChunk(
                db,
                getTimeseriesCollForDDLOps(db, db[collName]).getName(),
                [{"control.min.t": new Date(currentTimeStamp)}, {"control.min.t": MaxKey}],
                destinationShard,
                /*waitForDelete=*/ false,
            );
        }

        // Verify that the number of docs remain the same.
        assert.eq(db[collName].find().itcount(), db[this.nonShardCollName].find().itcount());
    };

    return $config;
});
