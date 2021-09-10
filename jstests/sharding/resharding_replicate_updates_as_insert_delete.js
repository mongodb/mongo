//
// Test to verify that updates that would change the resharding key value are replicated as an
// insert, delete pair.
// @tags: [
//   requires_fcv_47,
//   uses_atclustertime,
// ]
//

(function() {
'use strict';

load('jstests/libs/discover_topology.js');
load('jstests/sharding/libs/resharding_test_fixture.js');

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const testColl = reshardingTest.createShardedCollection({
    ns: 'test.foo',
    shardKeyPattern: {x: 1, s: 1},
    chunks: [
        {min: {x: MinKey, s: MinKey}, max: {x: 5, s: 5}, shard: donorShardNames[0]},
        {min: {x: 5, s: 5}, max: {x: MaxKey, s: MaxKey}, shard: donorShardNames[1]},
    ],
});

const docToUpdate = ({_id: 0, x: 2, s: 2, y: 2});
assert.commandWorked(testColl.insert(docToUpdate));

let retryableWriteTs;
let txnWriteTs;

const mongos = testColl.getMongo();
const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground(  //
    {
        newShardKeyPattern: {y: 1, s: 1},
        newChunks: [
            {min: {y: MinKey, s: MinKey}, max: {y: 5, s: 5}, shard: recipientShardNames[0]},
            {min: {y: 5, s: 5}, max: {y: MaxKey, s: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    (tempNs) => {
        // Wait for cloning to have at least started on the recipient shards to know that the donor
        // shards have begun including the "destinedRecipient" field in their oplog entries.
        const tempColl = mongos.getCollection(tempNs);
        assert.soon(() => tempColl.findOne(docToUpdate) !== null);

        assert.commandFailedWithCode(
            testColl.update({_id: 0, x: 2, s: 2}, {$set: {y: 10}}),
            ErrorCodes.IllegalOperation,
            'was able to update value under new shard key as ordinary write');

        const session = testColl.getMongo().startSession({retryWrites: true});
        const sessionColl =
            session.getDatabase(testColl.getDB().getName()).getCollection(testColl.getName());

        assert.commandFailedWithCode(
            sessionColl.update({_id: 0, x: 2, s: 2}, {$set: {y: 10}}, {multi: true}),
            ErrorCodes.InvalidOptions,
            'was able to update value under new shard key when {multi: true} specified');

        assert.commandFailedWithCode(
            sessionColl.update({_id: 0}, {$set: {y: 10}}),
            31025,
            'was able to update value under new shard key without specifying the full shard key ' +
                'in the query');

        let res;
        assert.soon(
            () => {
                res = sessionColl.update({_id: 0, x: 2, s: 2}, {$set: {y: 20, s: 3}});

                if (res.nModified === 1) {
                    assert.commandWorked(res);
                    retryableWriteTs = session.getOperationTime();
                    return true;
                }

                assert.commandFailedWithCode(res, [
                    ErrorCodes.StaleConfig,
                    ErrorCodes.NoSuchTransaction,
                    ErrorCodes.ShardCannotRefreshDueToLocksHeld
                ]);
                return false;
            },
            () => `was unable to update value under new shard key as retryable write: ${
                tojson(res)}`);

        assert.soon(() => {
            session.startTransaction();
            res = sessionColl.update({_id: 0, x: 2, s: 3}, {$set: {y: -30, s: 1}});

            if (res.nModified === 1) {
                session.commitTransaction();
                txnWriteTs = session.getOperationTime();
                return true;
            }

            // mongos will automatically retry the update as a pair of delete and insert commands in
            // a multi-document transaction. We permit NoSuchTransaction errors because it is
            // possible for the resharding operation running in the background to cause the shard
            // version to be bumped. The StaleConfig error won't be automatically retried by mongos
            // for the second statement in the transaction (the insert) and would lead to a
            // NoSuchTransaction error.
            assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
            session.abortTransaction();
            return false;
        }, () => `was unable to update value under new shard key in transaction: ${tojson(res)}`);
    });

const topology = DiscoverTopology.findConnectedNodes(mongos);
const donor0 = new Mongo(topology.shards[donorShardNames[0]].primary);
const donorOplogColl0 = donor0.getCollection('local.oplog.rs');

function assertOplogEntryIsDeleteInsertApplyOps(entry) {
    assert(entry.o.hasOwnProperty('applyOps'), entry);
    assert.eq(entry.o.applyOps.length, 2, entry);
    assert.eq(entry.o.applyOps[0].op, 'd', entry);
    assert.eq(entry.o.applyOps[0].ns, testColl.getFullName(), entry);
    assert.eq(entry.o.applyOps[1].op, 'i', entry);
    assert.eq(entry.o.applyOps[1].ns, testColl.getFullName(), entry);
}

const retryableWriteEntry = donorOplogColl0.findOne({ts: retryableWriteTs});
assert.neq(null, retryableWriteEntry, 'failed to find oplog entry for retryable write');
assertOplogEntryIsDeleteInsertApplyOps(retryableWriteEntry);

const txnWriteEntry = donorOplogColl0.findOne({ts: txnWriteTs});
assert.neq(null, txnWriteEntry, 'failed to find oplog entry for transaction');
assertOplogEntryIsDeleteInsertApplyOps(txnWriteEntry);

reshardingTest.teardown();
})();
