/**
 * Verify that the cloning phase of a resharding operation takes at least
 * reshardingMinimumOperationDurationMillis to complete. This will also indirectly verify that the
 * txnCloners were not started until after waiting for reshardingMinimumOperationDurationMillis to
 * elapse.
 *
 * @tags: [uses_atclustertime]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

function runTest(minimumOperationDurationMS, shouldReshardInPlace) {
    jsTest.log(`Running test for minimumReshardingDuration = ${
        minimumOperationDurationMS} and reshardInPlace = ${shouldReshardInPlace}`);

    const reshardingTest = new ReshardingTest({
        numDonors: 2,
        numRecipients: 2,
        reshardInPlace: shouldReshardInPlace,
        minimumOperationDurationMS: minimumOperationDurationMS
    });
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const sourceCollection = reshardingTest.createShardedCollection({
        ns: "reshardingDb.coll",
        shardKeyPattern: {oldKey: 1},
        chunks: [
            {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
            {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
        ],
    });

    // Test batched insert with multiple batches on shard 0, let it be one batch on shard 1.
    const rst0 = reshardingTest.getReplSetForShard(donorShardNames[0]);
    rst0.nodes.forEach(node => {assert.commandWorked(
                           node.adminCommand({setParameter: 1, internalInsertMaxBatchSize: 2}))});

    assert.commandWorked(sourceCollection.insert([
        {_id: "stays on shard0", oldKey: -10, newKey: -10, counter: 0},
        {_id: "moves to shard0", oldKey: 10, newKey: -10, counter: 0},
    ]));

    // We test both updates, which use 'u' oplog entries, and vectored inserts, which use 'applyOps'
    // oplog entries when featureFlagReplicateVectoredInsertsTransactionally is turned on.
    const mongos = sourceCollection.getMongo();
    const session = mongos.startSession({causalConsistency: false, retryWrites: false});
    const sessionCollection = session.getDatabase(sourceCollection.getDB().getName())
                                  .getCollection(sourceCollection.getName());
    const insertSession = mongos.startSession({causalConsistency: false, retryWrites: false});
    const insertSessionCollection = insertSession.getDatabase(sourceCollection.getDB().getName())
                                        .getCollection(sourceCollection.getName());
    const insertDuringReshardingSession =
        mongos.startSession({causalConsistency: false, retryWrites: false});
    const insertDuringReshardingSessionCollection =
        insertDuringReshardingSession.getDatabase(sourceCollection.getDB().getName())
            .getCollection(sourceCollection.getName());
    const updateCommand = {
        update: sourceCollection.getName(),
        updates: [
            {q: {_id: "stays on shard0"}, u: {$inc: {counter: 1}}},
            {q: {_id: "moves to shard0"}, u: {$inc: {counter: 1}}},
        ],
        txnNumber: NumberLong(1)
    };

    const insertCommand = {
        insert: sourceCollection.getName(),
        documents: [
            {_id: "ins_stays_on_shard0_0", oldKey: -20, newKey: -20, tag: "before"},
            {_id: "ins_stays_on_shard0_1", oldKey: -20, newKey: -20, tag: "before"},
            {_id: "ins_moves_to_shard1_0", oldKey: -20, newKey: 20, tag: "before"},
            {_id: "ins_moves_to_shard1_1", oldKey: -20, newKey: 20, tag: "before"},
            {_id: "ins_stays_on_shard1_0", oldKey: 20, newKey: 20, tag: "before"},
            {_id: "ins_stays_on_shard1_1", oldKey: 20, newKey: 20, tag: "before"},
            {_id: "ins_moves_to_shard0_0", oldKey: 20, newKey: -20, tag: "before"},
            {_id: "ins_moves_to_shard0_1", oldKey: 20, newKey: -20, tag: "before"},
        ],
        txnNumber: NumberLong(2)
    };
    const insertDuringReshardingCommand = {
        insert: sourceCollection.getName(),
        documents: [
            {_id: "ins_dur_stays_on_shard0_0", oldKey: -20, newKey: -20, tag: "during"},
            {_id: "ins_dur_stays_on_shard0_1", oldKey: -20, newKey: -20, tag: "during"},
            {_id: "ins_dur_moves_to_shard1_0", oldKey: -20, newKey: 20, tag: "during"},
            {_id: "ins_dur_moves_to_shard1_1", oldKey: -20, newKey: 20, tag: "during"},
            {_id: "ins_dur_stays_on_shard1_0", oldKey: 20, newKey: 20, tag: "during"},
            {_id: "ins_dur_stays_on_shard1_1", oldKey: 20, newKey: 20, tag: "during"},
            {_id: "ins_dur_moves_to_shard0_0", oldKey: 20, newKey: -20, tag: "during"},
            {_id: "ins_dur_moves_to_shard0_1", oldKey: 20, newKey: -20, tag: "during"},
        ],
        txnNumber: NumberLong(2)
    };

    function runRetryableWrites(
        phase, expectedUpdateErrorCode = ErrorCodes.OK, expectedInsertErrorCode = ErrorCodes.OK) {
        RetryableWritesUtil.runRetryableWrite(
            sessionCollection, updateCommand, expectedUpdateErrorCode);

        const updateDocs = sourceCollection.find({counter: {$exists: true}}).toArray();
        assert.eq(2, updateDocs.length, {updateDocs});

        for (const updateDoc of updateDocs) {
            assert.eq(1, updateDoc.counter, {
                message: `retryable write executed more than once ${phase}`,
                id: updateDoc._id,
                updateDocs
            });
        }

        // If an insert runs more than once, we'll get a DuplicateKeyError.
        RetryableWritesUtil.runRetryableWrite(
            insertSessionCollection, insertCommand, expectedInsertErrorCode);
        const insertDocs = sourceCollection.find({tag: "before"}).toArray();
        assert.eq(8, insertDocs.length, {insertDocs});

        if (phase != "before resharding" && phase != "during resharding") {
            // We only want to run these after the clone timestamp is chosen, because that
            // ensures they will be applied as oplog operations after cloning.
            RetryableWritesUtil.runRetryableWrite(insertDuringReshardingSessionCollection,
                                                  insertDuringReshardingCommand,
                                                  expectedInsertErrorCode);
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
            newShardKeyPattern: {newKey: 1},
            newChunks: [
                {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
                {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
            ],
        },
        () => {
            // Ideally, we want to start the timer right when the coordinator enters the cloning
            // stage. However, since the coordinator is running independently of this thread, it
            // is possible that any delays that occur in this thread can also cause the delay of
            // starting the timer. This has a consequence of getting an elapsed time that is shorter
            // than the minimumOperationDurationMS. To work around this, we start the timer earlier
            // with the trade off that it can add few extra seconds to the elapsed time. This is ok
            // as minimumOperationDurationMS is sufficiently large enough that we can confidently
            // say that the resharding coordinator waited for minimumOperationDurationMS.
            let startTime = Date.now();

            runRetryableWrites("during resharding");

            assert.soon(() => {
                const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                    ns: sourceCollection.getFullName()
                });

                return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
            });

            runRetryableWrites("during resharding after cloneTimestamp was chosen");

            assert.soon(() => {
                const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                    ns: sourceCollection.getFullName()
                });

                return coordinatorDoc !== null && coordinatorDoc.state === "cloning";
            });

            runRetryableWrites("during resharding when in coordinator in cloning state");

            assert.soon(() => {
                const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                    ns: sourceCollection.getFullName()
                });

                return coordinatorDoc !== null && coordinatorDoc.state === "applying";
            });

            const epsilon = 5000;
            const elapsed = Date.now() - startTime;
            assert.gt(elapsed, minimumOperationDurationMS - epsilon);
            if (FeatureFlagUtil.isPresentAndEnabled(mongos, "UpdateOneWithIdWithoutShardKey")) {
                runRetryableWrites("during resharding after collection cloning had finished");
            } else {
                runRetryableWrites("during resharding after collection cloning had finished",
                                   ErrorCodes.IncompleteTransactionHistory);
            }
        });

    if (FeatureFlagUtil.isPresentAndEnabled(mongos, "UpdateOneWithIdWithoutShardKey")) {
        runRetryableWrites(
            "after resharding", ErrorCodes.OK, ErrorCodes.IncompleteTransactionHistory);
    } else {
        runRetryableWrites("after resharding",
                           ErrorCodes.IncompleteTransactionHistory,
                           ErrorCodes.IncompleteTransactionHistory);
    }
    reshardingTest.teardown();
}
const minimumOperationDurationMS = 30000;
runTest(minimumOperationDurationMS, true);
runTest(minimumOperationDurationMS, false);
