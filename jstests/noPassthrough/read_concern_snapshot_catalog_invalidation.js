// Tests that snapshot reads return an error when accessing a collection whose metadata is invalid
// for the snapshot's point in time.
// @tags: [uses_transactions]
(function() {
"use strict";

load("jstests/libs/curop_helpers.js");      // For waitForCurOpByFailPoint().
load("jstests/libs/feature_flag_util.js");  // For FeatureFlagUtil.isEnabled().

const kDbName = "test";
const kCollName = "coll";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const testDB = rst.getPrimary().getDB(kDbName);
const adminDB = testDB.getSiblingDB("admin");
const coll = testDB.getCollection(kCollName);

function execCommand(cmd, expectSucceed) {
    if (expectSucceed) {
        return startParallelShell(
            "const session = db.getMongo().startSession();" +
                "const sessionDb = session.getDatabase('test');" +
                "session.startTransaction({readConcern: {level: 'snapshot'}});" +
                "const res = sessionDb.runCommand(" + tojson(cmd) + ");" +
                "assert.commandWorked(res);" +
                "session.endSession();",
            rst.ports[0]);
    } else {
        return startParallelShell(
            "const session = db.getMongo().startSession();" +
                "const sessionDb = session.getDatabase('test');" +
                "session.startTransaction({readConcern: {level: 'snapshot'}});" +
                "const res = sessionDb.runCommand(" + tojson(cmd) + ");" +
                "assert.commandFailedWithCode(res, [ErrorCodes.SnapshotUnavailable, ErrorCodes.WriteConflict]);" +
                "assert.eq(res.errorLabels, ['TransientTransactionError']);" +
                "session.endSession();",
            rst.ports[0]);
    }
}

function testCommand(cmd, curOpFilter, expectSucceed) {
    coll.drop({writeConcern: {w: "majority"}});
    assert.commandWorked(coll.insert({x: 1}, {writeConcern: {w: "majority"}}));

    // Start a command with readConcern "snapshot" that hangs after establishing a storage
    // engine transaction.
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: "hangAfterPreallocateSnapshot", mode: "alwaysOn"}));

    // Execute command in parallel shell. Read commands should work even if catalog changes has
    // occured since opening the snapshot.
    const awaitCommand = execCommand(cmd, expectSucceed);

    waitForCurOpByFailPointNoNS(testDB, "hangAfterPreallocateSnapshot", curOpFilter);

    // Rename the collection the command was executed against and then back to its original name.
    // This will move the collection's minimum visible timestamp to a point later than the
    // point-in-time referenced by the transaction snapshot.
    const tempColl = testDB.getName() + '.temp';
    assert.commandWorked(testDB.adminCommand({
        renameCollection: testDB.getName() + '.' + kCollName,
        to: tempColl,
        writeConcern: {w: "majority"},
    }));
    assert.commandWorked(testDB.adminCommand({
        renameCollection: tempColl,
        to: testDB.getName() + '.' + kCollName,
        writeConcern: {w: "majority"},
    }));

    // Disable the hang and check for parallel shell success. Success indicates that the command
    // failed due to collection metadata invalidation.
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: "hangAfterPreallocateSnapshot", mode: "off"}));

    awaitCommand();
}

testCommand({aggregate: kCollName, pipeline: [], cursor: {}},
            {"command.aggregate": kCollName, "command.readConcern.level": "snapshot"},
            true /*read is expected to succeed*/);
testCommand({delete: kCollName, deletes: [{q: {x: 1}, limit: 1}]},
            {"command.delete": kCollName, "command.readConcern.level": "snapshot"},
            false /*write is expected to fail*/);
testCommand({distinct: kCollName, key: "x"},
            {"command.distinct": kCollName, "command.readConcern.level": "snapshot"},
            true /*read is expected to succeed*/);
testCommand({find: kCollName},
            {"command.find": kCollName, "command.readConcern.level": "snapshot"},
            true /*read is expected to succeed*/);
testCommand({findAndModify: kCollName, query: {x: 1}, remove: true},
            {
                "command.findAndModify": kCollName,
                "command.remove": true,
                "command.readConcern.level": "snapshot"
            },
            false /*write is expected to fail*/);
testCommand({findAndModify: kCollName, query: {x: 1}, update: {$set: {x: 2}}},
            {
                "command.findAndModify": kCollName,
                "command.update.$set": {x: 2},
                "command.readConcern.level": "snapshot"
            },
            false /*write is expected to fail*/);
testCommand({insert: kCollName, documents: [{x: 1}]},
            {"command.insert": kCollName, "command.readConcern.level": "snapshot"},
            false /*write is expected to fail*/);
testCommand({update: kCollName, updates: [{q: {x: 1}, u: {$set: {x: 2}}}]},
            {"command.update": kCollName, "command.readConcern.level": "snapshot"},
            false /*write is expected to fail*/);

rst.stopSet();
})();
