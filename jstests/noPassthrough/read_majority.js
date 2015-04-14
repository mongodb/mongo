(function() {
"use strict";

// This test needs its own mongod since the snapshot names must be in increasing order and once you
// have a majority commit point it is impossible to go back to not having one.
var testServer = MongoRunner.runMongod();
var db = testServer.getDB("test");
var t = db.readMajority;

var errorCodes = {
    CommandNotSupported: 115,
    XXX_TEMP_NAME_ReadCommittedCurrentlyUnavailable: 134,
}

function assertNoReadMajoritySnapshotAvailable() {
    var res = t.runCommand('find', {batchSize: 2, $readMajorityTemporaryName: true});
    assert.commandFailed(res);
    assert.eq(res.code, errorCodes.XXX_TEMP_NAME_ReadCommittedCurrentlyUnavailable);
}

function getReadMajorityCursor() {
    var method = 'pcs';
    if (method == 'find') {
        // Doesn't work yet since find command ignores batchsize.
        var res = t.runCommand('find', {batchSize: 2, $readMajorityTemporaryName: true});
        assert.commandWorked(res);
        return new DBCommandCursor(db.getMongo(), res, 2);
    }
    else if (method == 'agg') {
        // Only works when DocumentSourceCursor batched fetching is disabled.
        return t.aggregate([], {$readMajorityTemporaryName: true, cursor: {batchSize: 2}});
    }
    else if (method == 'pcs') {
        // Always works.
        var res = t.runCommand('parallelCollectionScan', {numCursors: 1,
                                                          $readMajorityTemporaryName: true});
        assert.commandWorked(res);
        assert.eq(res.cursors.length, 1);
        return new DBCommandCursor(db.getMongo(), res.cursors[0], 2);
    }
}

//
// Actual Test
//

if (!db.serverStatus().storageEngine.supportsCommittedReads) {
    print("Skipping read_majority.js since storageEngine doesn't support it.");
    return;
}

assert.commandWorked(db.adminCommand({"makeSnapshot": Timestamp(1, 0)}));

for (var i = 0; i < 10; i++) { assert.writeOK(t.insert({_id: i, version: Timestamp(2, 0)})); }

assertNoReadMajoritySnapshotAvailable();

assert.commandWorked(db.adminCommand({"makeSnapshot": Timestamp(2, 0)}));

assertNoReadMajoritySnapshotAvailable();

assert.writeOK(t.update({}, {$set: {version: Timestamp(3, 0)}}, false, true));
assert.commandWorked(db.adminCommand({"makeSnapshot": Timestamp(3, 0)}));

assert.commandWorked(db.adminCommand({"setCommittedSnapshot": Timestamp(1, 0)}));

// Note: collection didn't exist in snapshot 0.
assert.eq(getReadMajorityCursor().itcount(), 0);

assert.commandWorked(db.adminCommand({"setCommittedSnapshot": Timestamp(2, 0)}));

var cursor = getReadMajorityCursor();
assert.eq(cursor.next().version, Timestamp(2, 0));
assert.eq(cursor.next().version, Timestamp(2, 0));
assert(!cursor.objsLeftInBatch());

assert.commandWorked(db.adminCommand({"setCommittedSnapshot": Timestamp(3, 0)}));

// This triggers a getMore which sees the new version.
assert.eq(cursor.next().version, Timestamp(3, 0));
assert.eq(cursor.next().version, Timestamp(3, 0));

MongoRunner.stopMongod(testServer);

}());
