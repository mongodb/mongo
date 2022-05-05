// Tests large multi-deletes in a transaction to trigger multi-applyOps transaction commit path.
// @tags: [requires_replication]
(function() {
"use strict";

load('jstests/libs/auto_retry_transaction_in_sharding.js');

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        // Ensure the storage engine cache can accommodate large transactions.
        wiredTigerCacheSizeGB: 3,
    },
});
rst.startSet();
rst.initiate();

// Use an intentionally short collection name to minimize the footprint of operations in the oplog.
const collName = "c";
const dbName = "test";
const testDB = rst.getPrimary().getDB(dbName);
const testColl = testDB[collName];

assert.commandWorked(testDB.createCollection(testColl.getName(), {writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false
};
const session = testDB.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];
const txnOpts = {
    writeConcern: {w: "majority"}
};

// Verify that a large number of small documents can be deleted when the transaction spans multiple
// "applyOps" entries.
jsTest.log("Prepopulate the collection.");
const numberOfDocuments = 256000;
const documents = [...new Array(numberOfDocuments).keys()].map(x => ({_id: x}));
assert.commandWorked(testColl.insert(documents, {writeConcern: {w: "majority"}}));

jsTest.log("Do a large multiple-result multi-delete.");
withTxnAndAutoRetryOnMongos(session, () => {
    // Verify that all documents are removed.
    let res = assert.commandWorked(sessionColl.remove({}, {justOne: false}));
    assert.eq(numberOfDocuments, res.nRemoved);
    res = sessionColl.find({});
    assert.sameMembers(res.toArray(), []);
}, txnOpts);

// Collection should be empty.
assert.eq(0, testColl.countDocuments({}));

rst.stopSet();
}());
