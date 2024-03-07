/**
 * Verify that retryable writes done before a move collection operation are
 * not retryable on a recipient that didn't have information for the write.
 *
 * @tags: [
 *  uses_atclustertime,
 *  featureFlagRecoverableShardsvrReshardCollectionCoordinator,
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagMoveCollection,
 *  featureFlagTrackUnshardedCollectionsUponCreation,
 *  multiversion_incompatible,
 * ]
 */

import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

function runTest(minimumOperationDurationMS, shouldReshardInPlace) {
    jsTest.log(`Running test for minimumReshardingDuration = ${
        minimumOperationDurationMS} and reshardInPlace = ${shouldReshardInPlace}`);

    const reshardingTest = new ReshardingTest({
        numDonors: 1,
        numRecipients: 1,
        reshardInPlace: shouldReshardInPlace,
        minimumOperationDurationMS: minimumOperationDurationMS
    });
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const sourceCollection = reshardingTest.createUnshardedCollection(
        {ns: "reshardingDb.coll", primaryShardName: donorShardNames[0]});

    assert.commandWorked(sourceCollection.insert([
        {_id: 0, counter: 0},
        {_id: 1, counter: 0},
    ]));

    const mongos = sourceCollection.getMongo();
    const session = mongos.startSession({causalConsistency: false, retryWrites: false});
    const sessionCollection = session.getDatabase(sourceCollection.getDB().getName())
                                  .getCollection(sourceCollection.getName());
    const updateCommand = {
        update: sourceCollection.getName(),
        updates: [
            {q: {_id: 0}, u: {$inc: {counter: 1}}},
            {q: {_id: 1}, u: {$inc: {counter: 1}}},
        ],
        txnNumber: NumberLong(1)
    };

    function runRetryableWrite(phase, expectedErrorCode = ErrorCodes.OK) {
        RetryableWritesUtil.runRetryableWrite(sessionCollection, updateCommand, expectedErrorCode);

        const docs = sourceCollection.find().toArray();
        assert.eq(2, docs.length, {docs});

        for (const doc of docs) {
            assert.eq(
                1,
                doc.counter,
                {message: `retryable write executed more than once ${phase}`, id: doc._id, docs});
        }
    }

    runRetryableWrite("before resharding");

    const recipientShardNames = reshardingTest.recipientShardNames;
    reshardingTest.withMoveCollectionInBackground({toShard: recipientShardNames[0]}, () => {
        assert.soon(() => {
            const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: sourceCollection.getFullName()
            });

            return coordinatorDoc !== null && coordinatorDoc.state === "applying";
        });

        // This will work since it goes to the donor which has all the info for this write.
        runRetryableWrite("during resharding after collection cloning had finished");
    });
    // This write will get targeted towards the older source shard, which will see that it already
    // executed the write without checking the shard version.
    runRetryableWrite("after resharding");

    // Flush the routing table, so mongos sends the write to the recipient. If we don't do this
    // flush, the older shard won't throw anything since the check that the write has already been
    // executed is before shard versioning checks.
    assert.commandWorked(mongos.adminCommand({flushRouterConfig: 1}));
    // This write will get targeted towards the destination shard which will have the
    // incomplete history.
    runRetryableWrite("after resharding", ErrorCodes.IncompleteTransactionHistory);

    reshardingTest.teardown();
}

const minimumOperationDurationMS = 1000;
runTest(minimumOperationDurationMS, false /* reshardInPlace */);
