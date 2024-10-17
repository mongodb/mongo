/**
 * Runs reshardCollection on a time-series collection and inserts concurrently.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 *   featureFlagReshardingForTimeseries,
 *   does_not_support_transactions,
 *   assumes_balancer_off,
 *   requires_fcv_80,
 *   # Some in memory variants will error because this test uses too much memory. As such, we do not
 *   # run this test on in-memory variants.
 *   requires_persistence,
 *   # TODO (SERVER-91251): Run this with stepdowns on TSAN.
 *   tsan_incompatible,
 * ]
 */
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/chunks.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

export const $config = (function() {
    const timeField = 'ts';
    const metaField = 'meta';

    const shardKeys = [
        {'meta.x': 1},
        {'meta.y': 1},
    ];

    const data = {
        shardKey: shardKeys[0],
        reshardingCount: 0,
    };

    function generateMetaFieldValueForInitialInserts(range) {
        return {x: Math.floor(Math.random() * range), y: Math.floor(Math.random() * range)};
    }

    const iterations = 50;
    const numInitialDocs = 5000;
    const kMaxReshardingExecutions = 4;

    function executeReshardTimeseries(db, collName, newShardKey) {
        print(`Started Resharding Timeseries Collection ${collName}. New Shard Key ${
            tojson(newShardKey)}`);

        let ns = db + "." + collName;
        let reshardCollectionCmd = {reshardCollection: ns, key: newShardKey, numInitialChunks: 1};
        if (TestData.runningWithShardStepdowns) {
            assert.commandWorkedOrFailedWithCode(db.adminCommand(reshardCollectionCmd),
                                                 [ErrorCodes.SnapshotUnavailable]);
        } else {
            assert.commandWorked(db.adminCommand(reshardCollectionCmd));
        }

        print(`Finished Resharding Timeseries Collection ${collName}. New Shard Key ${
            tojson(newShardKey)}`);
    }

    const states = {
        insert: function insert(db, collName) {
            print(`Inserting documents for collection ${collName}.`);
            const docs = [];
            for (let i = 0; i < 250; ++i) {
                docs.push({
                    [metaField]: generateMetaFieldValueForInitialInserts(15),
                    [timeField]: new Date(),
                });
            }

            assert.soon(() => {
                const res = db[collName].insert(docs);

                if (res.code == ErrorCodes.NoProgressMade) {
                    print(`No progress made while inserting documents. Retrying.`);
                    return false;
                }

                TimeseriesTest.assertInsertWorked(res);
                return true;
            });

            print(`Finished Inserting documents.`);
        },
        reshardTimeseries: function reshardTimeseries(db, collName) {
            const shouldContinueResharding = this.reshardingCount <= kMaxReshardingExecutions;
            if (this.tid === 0 && shouldContinueResharding) {
                let newShardKey;
                if (bsonWoCompare(this.shardKey, shardKeys[0]) === 0) {
                    newShardKey = shardKeys[1];
                } else {
                    newShardKey = shardKeys[0];
                }

                executeReshardTimeseries(db, collName, newShardKey);
                this.shardKey = newShardKey;
                this.reshardingCount += 1;
            }
        },
    };

    const transitions = {
        reshardTimeseries: {insert: 1},
        insert: {insert: .85, reshardTimeseries: .15},
    };

    function setup(db, collName, cluster) {
        db[collName].drop();

        assert.commandWorked(db.createCollection(
            collName, {timeseries: {metaField: metaField, timeField: timeField}}));
        cluster.shardCollection(db[collName], {'meta.x': 1}, false);

        const shards = Object.keys(cluster.getSerializedCluster().shards);
        const bucketNss = 'system.buckets.' + collName;
        ChunkHelper.splitChunkAt(db, bucketNss, {'meta.x': 5});

        ChunkHelper.moveChunk(db, bucketNss, [{'meta.x': MinKey}, {'meta.x': 5}], shards[0]);
        ChunkHelper.moveChunk(db, bucketNss, [{'meta.x': 5}, {'meta.x': MaxKey}], shards[1]);

        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < numInitialDocs; ++i) {
            const doc = {
                _id: new ObjectId(),
                [metaField]: generateMetaFieldValueForInitialInserts(10),
                [timeField]: new Date(),
            };
            bulk.insert(doc);
        }
        let res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(numInitialDocs, res.nInserted);
        assert.eq(100, db[bucketNss].countDocuments({}));
    }

    return {
        threadCount: 20,
        iterations: iterations,
        startState: 'reshardTimeseries',
        states: states,
        transitions: transitions,
        setup: setup,
        data: data
    };
})();
