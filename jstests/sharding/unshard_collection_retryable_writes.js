/**
 * Verify that retryable writes done before an unshard collection operation are
 * not retryable on a recipient if it didn't have information for the write.
 *
 * @tags: [
 *  uses_atclustertime,
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  featureFlagUnshardCollection,
 *  featureFlagTrackUnshardedCollectionsUponCreation,
 *  multiversion_incompatible,
 *  assumes_balancer_off,
 * ]
 */

import {RetryableWritesUtil} from "jstests/libs/retryable_writes_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

function runTest(minimumOperationDurationMS, shouldReshardInPlace) {
    jsTest.log(`Running test for minimumReshardingDuration = ${
        minimumOperationDurationMS} and reshardInPlace = ${shouldReshardInPlace}`);

    const reshardingTest = new ReshardingTest({
        numDonors: 2,
        numRecipients: 1,
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

    assert.commandWorked(sourceCollection.insert([
        {_id: 0, oldKey: -10, counter: 0},
        {_id: 1, oldKey: -5, counter: 0},
    ]));

    const mongos = sourceCollection.getMongo();
    const session = mongos.startSession({causalConsistency: false, retryWrites: false});
    const sessionCollection = session.getDatabase(sourceCollection.getDB().getName())
                                  .getCollection(sourceCollection.getName());
    const updateCommand = {
        update: sourceCollection.getName(),
        updates: [
            {q: {oldKey: -10}, u: {$inc: {counter: 1}}},
            {q: {oldKey: -5}, u: {$inc: {counter: 1}}},
        ],
        txnNumber: NumberLong(1)
    };

    function checkDocsConsistency() {
        const docs = sourceCollection.find().toArray();
        assert.eq(2, docs.length, {docs});

        for (const doc of docs) {
            assert.eq(1,
                      doc.counter,
                      {message: `retryable write executed more than once`, id: doc._id, docs});
        }
    }

    // before resharding
    let res =
        RetryableWritesUtil.runRetryableWrite(sessionCollection, updateCommand, ErrorCodes.OK);
    assert(res.nModified == 2, res);
    checkDocsConsistency();

    const recipientShardNames = reshardingTest.recipientShardNames;
    reshardingTest.withUnshardCollectionInBackground({toShard: recipientShardNames[0]}, () => {
        assert.soon(() => {
            const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: sourceCollection.getFullName()
            });

            return coordinatorDoc !== null && coordinatorDoc.state === "applying";
        });
        // oldKey update will target shard which has the info for the write.
        res =
            RetryableWritesUtil.runRetryableWrite(sessionCollection, updateCommand, ErrorCodes.OK);
        assert(res.nModified == 2, res);
        checkDocsConsistency();
    });

    // This write will get targeted towards the shard which does have the info for this write.
    res = RetryableWritesUtil.runRetryableWrite(sessionCollection, updateCommand, ErrorCodes.OK);
    assert(res.nModified == 2, res);
    checkDocsConsistency();

    // Flush the routing table, so mongos sends the write to the recipient. If we don't do this
    // flush, the older shard won't throw anything since the check that the write has already been
    // executed is before shard versioning checks.
    assert.commandWorked(mongos.adminCommand({flushRouterConfig: 1}));

    // This write will get targeted towards the recipient shard which does not have info for this
    // write.
    res = RetryableWritesUtil.runRetryableWrite(
        sessionCollection, updateCommand, ErrorCodes.IncompleteTransactionHistory);
    assert(res.writeErrors[0].code == 217, res);
    checkDocsConsistency();

    reshardingTest.teardown();
}

const minimumOperationDurationMS = 1000;
runTest(minimumOperationDurationMS, true);
runTest(minimumOperationDurationMS, false);
