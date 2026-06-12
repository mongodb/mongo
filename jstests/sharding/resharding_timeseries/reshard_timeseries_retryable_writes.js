/**
 * Timeseries variant of resharding_retryable_writes.js.
 *
 * Verify that retryable inserts behave correctly during timeseries resharding — inserts are not
 * duplicated and retryability history is preserved across the resharding lifecycle.
 * Uses a non-"meta" metaField name ("sensorId") to exercise the shard key translation path
 * (user-facing field -> internal bucket field).
 *
 * Note: sharded timeseries collections disallow {multi:false} updates, and {multi:true} updates
 * are not retryable, so this test covers only retryable inserts (unlike the non-timeseries analog
 * resharding_retryable_writes.js which also tests retryable updates).
 *
 * @tags: [
 *   uses_atclustertime,
 *   requires_fcv_80,
 *   multiversion_incompatible,
 * ]
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

function runTest(minimumOperationDurationMS, shouldReshardInPlace) {
    jsTest.log(
        `Running test for minimumReshardingDuration = ${
            minimumOperationDurationMS
        } and reshardInPlace = ${shouldReshardInPlace}`,
    );

    const reshardingTest = new ReshardingTest({
        numDonors: 2,
        numRecipients: 2,
        reshardInPlace: shouldReshardInPlace,
        minimumOperationDurationMS: minimumOperationDurationMS,
    });
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const timeseriesInfo = {timeField: "ts", metaField: "sensorId"};
    const sourceCollection = reshardingTest.createShardedCollection({
        ns: "reshardingDb.coll",
        // "sensorId.x" is the user-facing field; translated internally to {"meta.x": 1}.
        shardKeyPattern: {"sensorId.x": 1},
        chunks: [
            {min: {"meta.x": MinKey}, max: {"meta.x": 0}, shard: donorShardNames[0]},
            {min: {"meta.x": 0}, max: {"meta.x": MaxKey}, shard: donorShardNames[1]},
        ],
        shardCollOptions: {timeseries: timeseriesInfo},
    });
    // In viewful timeseries (FCV < 9.0), DDL operations use the system.buckets namespace.
    const sourceNs = getTimeseriesCollForDDLOps(
        sourceCollection.getDB(),
        sourceCollection,
    ).getFullName();

    // Test batched insert with multiple batches on shard 0, let it be one batch on shard 1.
    const rst0 = reshardingTest.getReplSetForShard(donorShardNames[0]);
    rst0.nodes.forEach((node) => {
        assert.commandWorked(node.adminCommand({setParameter: 1, internalInsertMaxBatchSize: 2}));
    });

    const mongos = sourceCollection.getMongo();
    const insertSession = mongos.startSession({causalConsistency: false, retryWrites: false});
    const insertSessionCollection = insertSession
        .getDatabase(sourceCollection.getDB().getName())
        .getCollection(sourceCollection.getName());
    const insertDuringReshardingSession = mongos.startSession({
        causalConsistency: false,
        retryWrites: false,
    });
    const insertDuringReshardingSessionCollection = insertDuringReshardingSession
        .getDatabase(sourceCollection.getDB().getName())
        .getCollection(sourceCollection.getName());

    const insertCommand = {
        insert: sourceCollection.getName(),
        documents: [
            {
                _id: "ins_stays_on_shard0_0",
                ts: new Date(),
                sensorId: {x: -20, y: -20},
                tag: "before",
            },
            {
                _id: "ins_stays_on_shard0_1",
                ts: new Date(),
                sensorId: {x: -20, y: -20},
                tag: "before",
            },
            {
                _id: "ins_moves_to_shard1_0",
                ts: new Date(),
                sensorId: {x: -20, y: 20},
                tag: "before",
            },
            {
                _id: "ins_moves_to_shard1_1",
                ts: new Date(),
                sensorId: {x: -20, y: 20},
                tag: "before",
            },
            {_id: "ins_stays_on_shard1_0", ts: new Date(), sensorId: {x: 20, y: 20}, tag: "before"},
            {_id: "ins_stays_on_shard1_1", ts: new Date(), sensorId: {x: 20, y: 20}, tag: "before"},
            {
                _id: "ins_moves_to_shard0_0",
                ts: new Date(),
                sensorId: {x: 20, y: -20},
                tag: "before",
            },
            {
                _id: "ins_moves_to_shard0_1",
                ts: new Date(),
                sensorId: {x: 20, y: -20},
                tag: "before",
            },
        ],
        txnNumber: NumberLong(1),
    };

    const insertDuringReshardingCommand = {
        insert: sourceCollection.getName(),
        documents: [
            {
                _id: "ins_dur_stays_on_shard0_0",
                ts: new Date(),
                sensorId: {x: -20, y: -20},
                tag: "during",
            },
            {
                _id: "ins_dur_stays_on_shard0_1",
                ts: new Date(),
                sensorId: {x: -20, y: -20},
                tag: "during",
            },
            {
                _id: "ins_dur_moves_to_shard1_0",
                ts: new Date(),
                sensorId: {x: -20, y: 20},
                tag: "during",
            },
            {
                _id: "ins_dur_moves_to_shard1_1",
                ts: new Date(),
                sensorId: {x: -20, y: 20},
                tag: "during",
            },
            {
                _id: "ins_dur_stays_on_shard1_0",
                ts: new Date(),
                sensorId: {x: 20, y: 20},
                tag: "during",
            },
            {
                _id: "ins_dur_stays_on_shard1_1",
                ts: new Date(),
                sensorId: {x: 20, y: 20},
                tag: "during",
            },
            {
                _id: "ins_dur_moves_to_shard0_0",
                ts: new Date(),
                sensorId: {x: 20, y: -20},
                tag: "during",
            },
            {
                _id: "ins_dur_moves_to_shard0_1",
                ts: new Date(),
                sensorId: {x: 20, y: -20},
                tag: "during",
            },
        ],
        txnNumber: NumberLong(2),
    };

    function runRetryableWrites(phase, expectedInsertErrorCode = ErrorCodes.OK) {
        // If an insert runs more than once, we'll get a DuplicateKeyError.
        RetryableWritesUtil.runRetryableWrite(
            insertSessionCollection,
            insertCommand,
            expectedInsertErrorCode,
        );
        const insertDocs = sourceCollection.find({tag: "before"}).toArray();
        assert.eq(8, insertDocs.length, {insertDocs});

        if (phase != "before resharding" && phase != "during resharding") {
            RetryableWritesUtil.runRetryableWrite(
                insertDuringReshardingSessionCollection,
                insertDuringReshardingCommand,
                ErrorCodes.OK,
            );
            const insertDuringDocs = sourceCollection.find({tag: "during"}).toArray();
            assert.eq(8, insertDuringDocs.length, {insertDuringDocs});
        } else {
            const insertDuringDocs = sourceCollection.find({tag: "during"}).toArray();
            assert.eq(0, insertDuringDocs.length, {insertDuringDocs});
        }
    }

    runRetryableWrites("before resharding");

    const recipientShardNames = reshardingTest.recipientShardNames;
    reshardingTest.withReshardingInBackground(
        {
            // "sensorId.y" is the user-facing field; translated internally to {"meta.y": 1}.
            newShardKeyPattern: {"sensorId.y": 1},
            newChunks: [
                {min: {"meta.y": MinKey}, max: {"meta.y": 0}, shard: recipientShardNames[0]},
                {min: {"meta.y": 0}, max: {"meta.y": MaxKey}, shard: recipientShardNames[1]},
            ],
        },
        () => {
            let startTime = Date.now();

            runRetryableWrites("during resharding");

            assert.soon(() => {
                const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                    ns: sourceNs,
                });
                return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
            });

            runRetryableWrites("during resharding after cloneTimestamp was chosen");

            assert.soon(() => {
                const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                    ns: sourceNs,
                });
                return coordinatorDoc !== null && coordinatorDoc.state === "cloning";
            });

            runRetryableWrites("during resharding when in coordinator in cloning state");

            assert.soon(() => {
                const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                    ns: sourceNs,
                });
                return coordinatorDoc !== null && coordinatorDoc.state === "applying";
            });

            const epsilon = 5000;
            const elapsed = Date.now() - startTime;
            assert.gt(elapsed, minimumOperationDurationMS - epsilon);

            runRetryableWrites("during resharding after collection cloning had finished");
        },
    );

    const insertExpectedCode = ErrorCodes.IncompleteTransactionHistory;
    runRetryableWrites("after resharding", insertExpectedCode);

    // After resharding, the shard key in config.collections uses the internal field name (meta.y),
    // not the user-facing metaField name (sensorId.y), verifying the translation path.
    const collEntry = mongos.getCollection("config.collections").findOne({_id: sourceNs});
    assert.neq(null, collEntry);
    assert.docEq({"meta.y": 1}, collEntry.key);

    reshardingTest.teardown();
}

const minimumOperationDurationMS = 30000;
runTest(minimumOperationDurationMS, true);
runTest(minimumOperationDurationMS, false);
