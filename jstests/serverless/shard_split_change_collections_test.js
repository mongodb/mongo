/**
 * Tests that a shard split handles change collections.
 * @tags: [requires_fcv_63, serverless]
 */

import {assertMigrationState, ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";
load("jstests/serverless/libs/change_collection_util.js");

const tenantIds = [ObjectId(), ObjectId()];
const donorRst = new ChangeStreamMultitenantReplicaSetTest({
    nodes: 3,
    nodeOptions: {setParameter: {shardSplitGarbageCollectionDelayMS: 0, ttlMonitorSleepSecs: 1}}
});

const test = new ShardSplitTest({quickGarbageCollection: true, donorRst});
test.addRecipientNodes();
test.donor.awaitSecondaryNodes();

const donorPrimary = test.getDonorPrimary();
const donorTenantConn =
    ChangeStreamMultitenantReplicaSetTest.getTenantConnection(donorPrimary.host, tenantIds[0]);
test.donor.setChangeStreamState(donorTenantConn, true);

// Open a change stream and insert documents into database.collection before the split
// starts.
const donorCursor = donorTenantConn.getDB("database").collection.watch([]);
const insertedDocs = [{_id: "tenant1_1"}, {_id: "tenant1_2"}, {_id: "tenant1_3"}];
donorTenantConn.getDB("database").collection.insertMany(insertedDocs);

const donorTenantSession = donorTenantConn.startSession({retryWrites: true});
const donorTenantSessionCollection = donorTenantSession.getDatabase("database").collection;
assert.commandWorked(donorTenantSessionCollection.insert({_id: "tenant1_4", w: "RETRYABLE"}));
assert.commandWorked(donorTenantSession.getDatabase("database").runCommand({
    findAndModify: "collection",
    query: {_id: "tenant1_4"},
    update: {$set: {updated: true}}
}));

// Start a transaction and perform some writes.
const donorTxnSession = donorTenantConn.getDB("database").getMongo().startSession();
donorTxnSession.startTransaction();
donorTxnSession.getDatabase("database").collection.insertOne({_id: "tenant1_in_transaction_1"});
donorTxnSession.getDatabase("database").collection.updateOne({_id: "tenant1_in_transaction_1"}, {
    $set: {updated: true}
});
donorTxnSession.commitTransaction();
donorTxnSession.endSession();

// Get the first entry from the change stream cursor and grab the resume token.
assert.eq(donorCursor.hasNext(), true);
const {_id: resumeToken} = donorCursor.next();

const operation = test.createSplitOperation(tenantIds);
assert.commandWorked(operation.commit());
assertMigrationState(donorPrimary, operation.migrationId, "committed");

let errCode;
try {
    donorTenantConn.getDB("database").collection.watch([]);
} catch (err) {
    errCode = err.code;
}
assert.eq(errCode,
          ErrorCodes.TenantMigrationCommitted,
          "Opening a change stream on the donor after completion of a shard split should fail.");

operation.forget();

const recipientRst = test.getRecipient();
const recipientPrimary = recipientRst.getPrimary();

const recipientPrimaryTenantConn = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    recipientPrimary.host, tenantIds[0], tenantIds[0].str);

// Running ChangeStreamMultitenantReplicaSetTest.getTenantConnection will create a user on the
// primary. Await replication so that we can use the same user on secondaries.
recipientRst.awaitReplication();

const recipientSecondaryConns = recipientRst.getSecondaries().map(
    node => ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
        node.host, tenantIds[0], tenantIds[0].str));

// Resume the change stream on all Recipient nodes.
const cursors = [recipientPrimaryTenantConn, ...recipientSecondaryConns].map(
    conn => conn.getDB("database").collection.watch([], {resumeAfter: resumeToken}));

[{_id: "tenant1_2", operationType: "insert"},
 {_id: "tenant1_3", operationType: "insert"},
 {_id: "tenant1_4", operationType: "insert"},
 {_id: "tenant1_4", operationType: "update"},
 {_id: "tenant1_in_transaction_1", operationType: "insert"},
 {_id: "tenant1_in_transaction_1", operationType: "update"},
].forEach(expectedEvent => {
    cursors.forEach(cursor => {
        assert.soon(() => cursor.hasNext());
        const changeEvent = cursor.next();
        assert.eq(changeEvent.documentKey._id, expectedEvent._id);
        assert.eq(changeEvent.operationType, expectedEvent.operationType);
    });
});

test.cleanupSuccesfulCommitted(operation.migrationId, tenantIds);
test.stop();
