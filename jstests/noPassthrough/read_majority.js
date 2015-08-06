(function() {
"use strict";

// This test needs its own mongod since the snapshot names must be in increasing order and once you
// have a majority commit point it is impossible to go back to not having one.
var testServer = MongoRunner.runMongod({setParameter: 'testingSnapshotBehaviorInIsolation=true'});
var db = testServer.getDB("test");
var t = db.readMajority;

var errorCodes = {
    CommandNotSupported: 115,
    ReadConcernMajorityNotAvailableYet: 134,
}

function assertNoReadMajoritySnapshotAvailable() {
    var res = t.runCommand('find', {batchSize: 2, readConcern: {level: "majority"}});
    assert.commandFailed(res);
    assert.eq(res.code, errorCodes.ReadConcernMajorityNotAvailableYet);
}

function getReadMajorityCursor() {
    var res = t.runCommand('find', {batchSize: 2, readConcern: {level: "majority"}});
    assert.commandWorked(res);
    return new DBCommandCursor(db.getMongo(), res, 2);
}

//
// Actual Test
//

if (!db.serverStatus().storageEngine.supportsCommittedReads) {
    print("Skipping read_majority.js since storageEngine doesn't support it.");
    return;
}

assert.commandWorked(db.runCommand({create: "readMajority"}));
assert.commandWorked(db.adminCommand({"makeSnapshot": NumberLong(1)}));

for (var i = 0; i < 10; i++) { assert.writeOK(t.insert({_id: i, version: 2})); }

assertNoReadMajoritySnapshotAvailable();

assert.commandWorked(db.adminCommand({"makeSnapshot": NumberLong(2)}));

assertNoReadMajoritySnapshotAvailable();

assert.writeOK(t.update({}, {$set: {version: 3}}, false, true));
assert.commandWorked(db.adminCommand({"makeSnapshot": NumberLong(3)}));

assert.commandWorked(db.adminCommand({"setCommittedSnapshot": NumberLong(1)}));

// Note: collection didn't exist in snapshot 0.
assert.eq(getReadMajorityCursor().itcount(), 0);

assert.commandWorked(db.adminCommand({"setCommittedSnapshot": NumberLong(2)}));

var cursor = getReadMajorityCursor();
assert.eq(cursor.next().version, 2);
assert.eq(cursor.next().version, 2);
assert(!cursor.objsLeftInBatch());

assert.commandWorked(db.adminCommand({"setCommittedSnapshot": NumberLong(3)}));

// This triggers a getMore which sees the new version.
assert.eq(cursor.next().version, 3);
assert.eq(cursor.next().version, 3);

MongoRunner.stopMongod(testServer);

}());
