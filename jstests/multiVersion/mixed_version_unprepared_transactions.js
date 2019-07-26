/**
 * Tests that a mixed version replica set is able to perform unprepared transactions.
 */

(function() {
jsTest.log("Starting a mixed version replica set.");

TestData.replSetFeatureCompatibilityVersion = '4.0';
const rst = new ReplSetTest({
    nodes: [
        {binVersion: 'latest'},
        {binVersion: 'latest'},
        {binVersion: 'latest', rsConfig: {priority: 0, votes: 0}},
    ]
});
rst.startSet();
rst.initiate();
// A 4.2 binVersion primary with empty data files will set FCV to 4.2 when elected. This will
// cause an IncompatibleServerVersion error when connecting with a 4.0 binVersion node.
// Therefore, we wait until the replica set is initiated with FCV4.0 before switching the
// binVersion to 4.0.
rst.restart(1, {binVersion: '4.0'});

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const collName = 'mixed_version_transactions';
const testColl = testDB.getCollection(collName);

testColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase('test');
const sessionColl = sessionDB.getCollection(collName);

jsTestLog("Start a transaction and insert a document and then commit.");
session.startTransaction();
const doc1 = {
    _id: 1
};
assert.commandWorked(sessionColl.insert(doc1));
assert.commandWorked(session.commitTransaction_forTesting());
assert.eq(doc1, sessionColl.findOne(doc1));

jsTestLog("Start a transaction and insert a document and then abort.");
session.startTransaction();
const doc2 = {
    _id: 2
};
assert.commandWorked(sessionColl.insert(doc2));
assert.commandWorked(session.abortTransaction_forTesting());
assert.eq(null, sessionColl.findOne(doc2));

jsTestLog("Have the node on 4.0 binVersion become the new primary.");
rst.stepUp(rst.nodes[1]);
const newPrimary = rst.getPrimary();

const newSession = newPrimary.startSession({causalConsistency: false});
const newSessionDB = newSession.getDatabase('test');
const newSessionColl = newSessionDB.getCollection(collName);

jsTestLog("Start a transaction and insert a document and then commit.");
newSession.startTransaction();
assert.commandWorked(newSessionColl.insert(doc2));
assert.commandWorked(newSession.commitTransaction_forTesting());
assert.eq(doc2, newSessionColl.findOne(doc2));

jsTestLog("Start a transaction and insert a document and then abort.");
newSession.startTransaction();
const doc3 = {
    _id: 3
};
assert.commandWorked(newSessionColl.insert(doc3));
assert.commandWorked(newSession.abortTransaction_forTesting());
assert.eq(null, newSessionColl.findOne(doc3));

rst.stopSet();
})();