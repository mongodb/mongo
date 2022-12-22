// Tests that the data cloning phase of initial sync does not clone the change collection documents
// and when the initial sync has completed the change collection and oplog entries are exactly same
// in the new secondary.
// @tags: [
//   requires_fcv_62,
// ]
//
(function() {
"use strict";

// For waitForFailPoint.
load("jstests/libs/fail_point_util.js");
// For verifyChangeCollectionEntries and ChangeStreamMultitenantReplicaSetTest.
load("jstests/serverless/libs/change_collection_util.js");

const replSetTest = new ChangeStreamMultitenantReplicaSetTest({nodes: 1});
const primary = replSetTest.getPrimary();

// User id and the associated tenant id.
const userInfo = {
    tenantId: ObjectId(),
    user: ObjectId().str
};

// Create a connection to the primary node for the tenant.
const tenantPrimaryNode = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    primary.host, userInfo.tenantId, userInfo.user);

// Enable the change stream for the tenant.
replSetTest.setChangeStreamState(tenantPrimaryNode, true);

// Get the change collection on the primary node for the tenant.
const primaryChangeColl = tenantPrimaryNode.getDB("config").system.change_collection;

const mdbStockPriceDoc = {
    _id: "mdb",
    price: 250
};

// The document 'mdbStockPriceDoc' is inserted before starting the initial sync. As such the
// document 'mdbStockPriceDoc' should not be cloned in the secondary after initial sync is complete.
assert.commandWorked(tenantPrimaryNode.getDB("test").stockPrice.insert(mdbStockPriceDoc));
assert.eq(primaryChangeColl.find({o: mdbStockPriceDoc}).toArray().length, 1);

// Add a new secondary to the replica set and block the initial sync after the data cloning is done.
const secondary = replSetTest.add({
    setParameter: {
        // Hang after the data cloning phase is completed.
        "failpoint.initialSyncHangAfterDataCloning": tojson({mode: "alwaysOn"})
    }
});
replSetTest.reInitiate();

// Wait for the cloning phase to complete. The cloning phase should not clone documents of the
// change collection from the primary.
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: "initialSyncHangAfterDataCloning",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

const tslaStockPriceDoc = {
    _id: "tsla",
    price: 650
};

// The document 'tslaStockPriceDoc' is inserted in the primary after the data cloning phase has
// completed, as such this should be inserted in the secondary's change change collection.
assert.commandWorked(tenantPrimaryNode.getDB("test").stockPrice.insert(tslaStockPriceDoc));
assert.eq(primaryChangeColl.find({o: tslaStockPriceDoc}).toArray().length, 1);

// Unblock the initial sync process.
assert.commandWorked(secondary.getDB("test").adminCommand(
    {configureFailPoint: "initialSyncHangAfterDataCloning", mode: "off"}));

// Wait for the initial sync to complete.
replSetTest.waitForState(secondary, ReplSetTest.State.SECONDARY);

// Create a connection to the secondary node for the tenant.
const tenantSecondaryNode = ChangeStreamMultitenantReplicaSetTest.getTenantConnection(
    secondary.host, userInfo.tenantId, userInfo.user);

// Verify that the document 'mdbStockPriceDoc' does not exist and the document 'tslaStockPriceDoc'
// exists in the secondary's change collection.
const changeCollDocs =
    tenantSecondaryNode.getDB("config")
        .system.change_collection.find({$or: [{o: mdbStockPriceDoc}, {o: tslaStockPriceDoc}]})
        .toArray();
assert.eq(changeCollDocs.length, 1);
assert.eq(changeCollDocs[0].o, tslaStockPriceDoc);

// Get the timestamp of the first and the last entry from the secondary's oplog.
const oplogDocs = secondary.getDB("local").oplog.rs.find().toArray();
assert.gt(oplogDocs.length, 0);
const startOplogTimestamp = oplogDocs[0].ts;
const endOplogTimestamp = oplogDocs.at(-1).ts;

// The change collection gets created at the data cloning phase and documents are written to the
// oplog only after the data cloning is done. And so, the change collection already exists in place
// to capture oplog entries. As such, the change collection entries and the oplog entries for
// timestamp range ('startOplogTimestamp', 'endOplogTimestamp'] must be the same.
verifyChangeCollectionEntries(secondary, startOplogTimestamp, endOplogTimestamp, userInfo.tenantId);

// The state of the change collection after the initial sync is not consistent with the primary.
// This is because the change collection's data is never cloned to the secondary, only it's creation
// is cloned. As such, we will skip the db hash check on the change collection.
replSetTest.stopSet(undefined /* signal */, undefined /* forRestart */, {skipCheckDBHashes: true});
})();
