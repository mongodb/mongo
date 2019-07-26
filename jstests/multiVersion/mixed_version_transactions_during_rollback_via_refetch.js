/**
 * Tests that a 4.2 binVersion node with FCV4.0 is able to perform rollback-via-refetch with an
 * unprepared transaction against a node with 4.0 binVersion.
 */

(function() {
load("jstests/replsets/libs/rollback_test.js");

jsTest.log("Starting a mixed version replica set.");

TestData.replSetFeatureCompatibilityVersion = '4.0';
const rst = new ReplSetTest({
    nodes: [
        {binVersion: 'latest'},
        {binVersion: 'latest'},
        {binVersion: 'latest', rsConfig: {priority: 0}},
    ],
    useBridge: true,
    nodeOptions: {enableMajorityReadConcern: "false"}
});
rst.startSet();
const config = rst.getReplSetConfig();
config.settings = {
    chainingAllowed: false
};
rst.initiate(config);
// A 4.2 binVersion primary with empty data files will set FCV to 4.2 when elected. This will
// cause an IncompatibleServerVersion error when connecting with a 4.0 binVersion node.
// Therefore, we wait until the replica set is initiated with FCV4.0 before switching the
// binVersion to 4.0.
rst.restart(1, {binVersion: '4.0'});

const collName = 'mixed_version_transactions_during_rollback_via_refetch';
rst.getPrimary().getDB('test').getCollection(collName).drop({writeConcern: {w: "majority"}});
assert.commandWorked(
    rst.getPrimary().getDB('test').createCollection(collName, {writeConcern: {w: "majority"}}));

rst.awaitReplication();
const rollbackTest = new RollbackTest(collName, rst);

const primary = rollbackTest.getPrimary();

const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase('test');
const sessionColl = sessionDB.getCollection(collName);

jsTestLog("Start a transaction and insert a document.");
session.startTransaction();
const doc1 = {
    _id: 1
};
assert.commandWorked(sessionColl.insert(doc1));
assert.eq(doc1, sessionColl.findOne(doc1));

// Stop replication from the current primary.
rollbackTest.transitionToRollbackOperations();
jsTestLog("Commit the transaction. This transaction is expected to be rolled back.");
assert.commandWorked(session.commitTransaction_forTesting());

// Step down current primary and elect a node that lacks the transaction oplog entry.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

rollbackTest.transitionToSyncSourceOperationsDuringRollback();

rollbackTest.transitionToSteadyStateOperations();

// Assert that the document has been rolled back.
assert.eq(null, sessionColl.findOne(doc1));

rollbackTest.stop();
})();