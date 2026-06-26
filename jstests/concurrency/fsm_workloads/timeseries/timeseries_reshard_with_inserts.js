/**
 * Runs reshardCollection on a time-series collection and inserts concurrently.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 *   does_not_support_transactions,
 *   assumes_balancer_off,
 *   requires_fcv_80,
 *   # Some in memory variants will error because this test uses too much memory. As such, we do not
 *   # run this test on in-memory variants.
 *   requires_persistence,
 *   # TODO (SERVER-91251): Run this with stepdowns on TSAN.
 *   tsan_incompatible,
 *   # This test relies on default timeseries parameters for countDocuments.
 *   does_not_support_config_fuzzer,
 * ]
 */
import {ChunkHelper} from "jstests/concurrency/fsm_workload_helpers/chunks.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {isSlowBuild} from "jstests/sharding/libs/sharding_util.js";

export const $config = (function () {
    // This test manually shards the collection.
    TestData.shardCollectionProbability = 0;

    const timeField = "ts";
    const metaField = "meta";

    const shardKeys = [{"meta.x": 1}, {"meta.y": 1}];

    const data = {
        shardKey: shardKeys[0],
        reshardingCount: 0,
        numDocsPerInsert: 250,
        originalValidationParams: null,
    };

    function setReshardingServerParameters(cluster, params) {
        const previous = {};
        const apply = (adminDb) => {
            for (const [name, value] of Object.entries(params)) {
                const res = assert.commandWorked(
                    adminDb.adminCommand({setParameter: 1, [name]: value}),
                );
                previous[name] = res.was;
            }
        };
        cluster.executeOnConfigNodes(apply);
        cluster.executeOnMongodNodes(apply);
        return previous;
    }

    function generateMetaFieldValueForInitialInserts(range) {
        return {x: Math.floor(Math.random() * range), y: Math.floor(Math.random() * range)};
    }

    const iterations = 100;
    const numInitialDocs = 5000;
    const kMaxReshardingExecutions = 4;

    function executeReshardTimeseries(db, collName, newShardKey) {
        print(
            `Started Resharding Timeseries Collection ${collName}. New Shard Key ${tojson(newShardKey)}`,
        );

        let ns = db + "." + collName;
        let reshardCollectionCmd = {reshardCollection: ns, key: newShardKey, numInitialChunks: 1};
        if (TestData.runningWithShardStepdowns) {
            assert.soonRetryOnAcceptableErrors(
                () => {
                    assert.commandWorkedOrFailedWithCode(db.adminCommand(reshardCollectionCmd), [
                        ErrorCodes.SnapshotUnavailable,
                    ]);
                    return true;
                },
                ErrorCodes.FailedToSatisfyReadPreference,
                "reshardCollection should eventually succeed after primary elections",
            );
        } else {
            assert.commandWorked(db.adminCommand(reshardCollectionCmd));
        }

        print(
            `Finished Resharding Timeseries Collection ${collName}. New Shard Key ${tojson(newShardKey)}`,
        );
    }

    const states = {
        insert: function insert(db, collName) {
            print(`Inserting documents for collection ${collName}.`);
            const docs = [];
            for (let i = 0; i < this.numDocsPerInsert; ++i) {
                docs.push({
                    [metaField]: generateMetaFieldValueForInitialInserts(15),
                    [timeField]: new Date(),
                });
            }

            retryOnRetryableError(
                () => {
                    TimeseriesTest.assertInsertWorked(db[collName].insert(docs));
                },
                100 /* numRetries */,
                undefined /* sleepMs */,
                [ErrorCodes.NoProgressMade],
            );

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
        insert: {insert: 0.85, reshardTimeseries: 0.15},
    };

    function teardown(_db, _collName, cluster) {
        if (data.originalValidationParams !== null) {
            setReshardingServerParameters(cluster, data.originalValidationParams);
        }
    }

    function setup(db, collName, cluster) {
        const reduceLoadForValidation =
            TestData.runningWithShardStepdowns &&
            isSlowBuild(db.getMongo()) &&
            FeatureFlagUtil.isEnabled(db, "ReshardingVerification");
        if (reduceLoadForValidation) {
            // On slow builds with stepdowns, the verification monitor is interrupted before completing its oplog scan.
            // Reduce write volume to allow more frequent checkpoints.
            this.numDocsPerInsert = 100;

            // Limit the critical section to 30 minutes (vs the 24-hour test default) with shorter batch limits
            // and a 1% verification wait. This allows resharding to skip validation if it takes too long.
            const validationParamsUnderStepdowns = {
                reshardingVerificationChangeStreamsEventsBatchTimeLimitSeconds: 4,
                reshardingCriticalSectionTimeoutMillis: 30 * 60 * 1000,
                reshardingVerificationDeltaWaitRemainingCriticalSectionPercent: 1,
            };
            data.originalValidationParams = setReshardingServerParameters(
                cluster,
                validationParamsUnderStepdowns,
            );
        }

        db[collName].drop();

        assert.commandWorked(
            db.createCollection(collName, {
                timeseries: {metaField: metaField, timeField: timeField},
            }),
        );
        cluster.shardCollection(db[collName], {"meta.x": 1}, false);

        const shards = Object.keys(cluster.getSerializedCluster().shards);
        ChunkHelper.splitChunkAt(db, getTimeseriesCollForDDLOps(db, db[collName]).getName(), {
            "meta.x": 8,
        });

        ChunkHelper.moveChunk(
            db,
            getTimeseriesCollForDDLOps(db, db[collName]).getName(),
            [{"meta.x": MinKey}, {"meta.x": 8}],
            shards[0],
        );
        ChunkHelper.moveChunk(
            db,
            getTimeseriesCollForDDLOps(db, db[collName]).getName(),
            [{"meta.x": 8}, {"meta.x": MaxKey}],
            shards[1],
        );

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
        assert.eq(
            100,
            getTimeseriesCollForRawOps(db, db[collName]).countDocuments(
                {},
                getRawOperationSpec(db),
            ),
        );
    }

    return {
        threadCount: 20,
        iterations: iterations,
        startState: "reshardTimeseries",
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        data: data,
    };
})();
