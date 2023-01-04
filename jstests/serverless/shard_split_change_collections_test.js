/**
 * Tests that a shard split handles change collections and cluster parameters.
 * @tags: [requires_fcv_62, serverless]
 */

import {assertMigrationState, ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";
load("jstests/libs/cluster_server_parameter_utils.js");
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

// Set a cluster parameter before the split starts.
assert.commandWorked(donorPrimary.getDB("admin").runCommand(tenantCommand(
    {setClusterParameter: {"changeStreams": {"expireAfterSeconds": 7200}}}, tenantIds[0])));

// Open a change stream and insert documents into database.collection before the split
// starts.
const donorCursor = donorTenantConn.getDB("database").collection.watch([]);
const insertedDocs = [{n: 0}, {n: 1}, {n: 2}];
donorTenantConn.getDB("database").collection.insertMany(insertedDocs);

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

const recipientTenantConn =
    ChangeStreamMultitenantReplicaSetTest.getTenantConnection(recipientPrimary.host, tenantIds[0]);

// Resume the change stream on the Recipient.
const recipientCursor =
    recipientTenantConn.getDB("database").collection.watch([], {resumeAfter: resumeToken});
assert.eq(recipientCursor.hasNext(), true);
let doc = recipientCursor.next();
assert.eq(doc.operationType, "insert");
assert.eq(doc.fullDocument.n, insertedDocs[1].n);
assert.eq(recipientCursor.hasNext(), true);
doc = recipientCursor.next();
assert.eq(doc.operationType, "insert");
assert.eq(doc.fullDocument.n, insertedDocs[2].n);
assert.eq(recipientCursor.hasNext(), false);

const {clusterParameters} = assert.commandWorked(recipientPrimary.getDB("admin").runCommand(
    tenantCommand({getClusterParameter: ["changeStreams"]}, tenantIds[0])));
const [changeStreamsClusterParameter] = clusterParameters;
assert.eq(changeStreamsClusterParameter.expireAfterSeconds, 7200);

test.cleanupSuccesfulCommitted(operation.migrationId, tenantIds);
test.stop();
