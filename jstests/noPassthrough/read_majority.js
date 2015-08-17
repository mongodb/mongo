load('jstests/libs/analyze_plan.js');

(function() {
"use strict";

// This test needs its own mongod since the snapshot names must be in increasing order and once you
// have a majority commit point it is impossible to go back to not having one.
var testServer = MongoRunner.runMongod({setParameter: 'testingSnapshotBehaviorInIsolation=true'});
var db = testServer.getDB("test");
var t = db.readMajority;

function assertNoReadMajoritySnapshotAvailable() {
    var res = t.runCommand('find',
                           {batchSize: 2, readConcern: {level: "majority"}, maxTimeMS: 1000});
    assert.commandFailed(res);
    assert.eq(res.code, ErrorCodes.ExceededTimeLimit);
}

function getReadMajorityCursor() {
    var res = t.runCommand('find', {batchSize: 2, readConcern: {level: "majority"}});
    assert.commandWorked(res);
    return new DBCommandCursor(db.getMongo(), res, 2);
}

function getReadMajorityExplainPlan(query) {
    var res = db.runCommand({explain: {find: t.getName(), filter: query},
                             readConcern: {level: "majority"}});
    return assert.commandWorked(res).queryPlanner.winningPlan;
}

//
// Actual Test
//

if (!db.serverStatus().storageEngine.supportsCommittedReads) {
    print("Skipping read_majority.js since storageEngine doesn't support it.");
    return;
}

var snapshot1 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
assert.commandWorked(db.runCommand({create: "readMajority"}));
var snapshot2 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;

for (var i = 0; i < 10; i++) { assert.writeOK(t.insert({_id: i, version: 3})); }

assertNoReadMajoritySnapshotAvailable();

var snapshot3 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;

assertNoReadMajoritySnapshotAvailable();

assert.writeOK(t.update({}, {$set: {version: 4}}, false, true));
var snapshot4 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;


// Collection didn't exist in snapshot 1.
assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot1}));
assertNoReadMajoritySnapshotAvailable();

// Collection existed but was empty in snapshot 2.
assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot2}));
assert.eq(getReadMajorityCursor().itcount(), 0);

// In snapshot 3 the collection was filled with {version: 3} documents.
assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot3}));
assert.eq(getReadMajorityCursor().itcount(), 10);
var cursor = getReadMajorityCursor(); // Note: uses batchsize=2.
assert.eq(cursor.next().version, 3);
assert.eq(cursor.next().version, 3);
assert(!cursor.objsLeftInBatch());

// In snapshot 4 the collection was filled with {version: 3} documents.
assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot4}));

// This triggers a getMore which sees the new version.
assert.eq(cursor.next().version, 4);
assert.eq(cursor.next().version, 4);

// Adding an index doesn't bump the min snapshot for a collection, but it can't be used for queries
// yet.
t.ensureIndex({version: 1});
assert.eq(getReadMajorityCursor().itcount(), 10);
assert(!isIxscan(getReadMajorityExplainPlan({version: 1})));

// To use the index, a snapshot created after the index was completed must be marked committed.
var snapshot5 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
assert(!isIxscan(getReadMajorityExplainPlan({version: 1})));
assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot5}));
assert(isIxscan(getReadMajorityExplainPlan({version: 1})));

// Dropping an index does bump the min snapshot.
t.dropIndex({version: 1});
assertNoReadMajoritySnapshotAvailable();

// To use the collection again, a snapshot created after the dropIndex must be marked committed.
var snapshot6 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
assertNoReadMajoritySnapshotAvailable();
assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot6}));
assert.eq(getReadMajorityCursor().itcount(), 10);

// Dropping the collection is visible in the committed snapshot, even though it hasn't been marked
// committed yet. This is allowed by the current specification even though it violates strict
// read-committed semantics since we don't guarantee them on metadata operations.
t.drop();
assert.eq(getReadMajorityCursor().itcount(), 0);

// Creating a new collection with the same name hides the collection until that operation is in the
// committed view.
t.insert({_id:0, version: 7});
assertNoReadMajoritySnapshotAvailable();
var snapshot7 = assert.commandWorked(db.adminCommand("makeSnapshot")).name;
assertNoReadMajoritySnapshotAvailable();
assert.commandWorked(db.adminCommand({"setCommittedSnapshot": snapshot7}));
assert.eq(getReadMajorityCursor().itcount(), 1);

MongoRunner.stopMongod(testServer);
}());
