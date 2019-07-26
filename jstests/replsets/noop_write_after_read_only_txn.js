// Do a transaction and commit with w: majority. Confirm that if there are no writes in the
// transaction, there is a noop write at the end, and confirm that commitTransaction awaits
// writeConcern majority.
//
// @tags: [uses_transactions]
(function() {
"use strict";
load('jstests/libs/write_concern_util.js');

const name = "noop_write_after_read_only_txn";
const rst = new ReplSetTest({
    name: name,
    nodes: [{}, {rsConfig: {priority: 0}}],
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "test";
const testDB = primary.getDB(dbName);

// Set up the collection.
testDB.runCommand({drop: name, writeConcern: {w: "majority"}});
assert.commandWorked(testDB.getCollection(name).insert({}, {writeConcern: {w: "majority"}}));

function runTest({readConcernLevel, shouldWrite, provokeWriteConcernError}) {
    jsTestLog(`Read concern level "${readConcernLevel}", shouldWrite: ${
        shouldWrite}, provokeWriteConcernError: ${provokeWriteConcernError}`);

    const session = primary.startSession();
    const sessionDB = session.getDatabase(dbName);
    const txnOptions = {writeConcern: {w: "majority"}};
    if (readConcernLevel)
        txnOptions.readConcern = {level: readConcernLevel};

    if (provokeWriteConcernError)
        txnOptions.writeConcern.wtimeout = 1000;

    session.startTransaction(txnOptions);
    assert.commandWorked(sessionDB.runCommand({find: name}));
    if (shouldWrite)
        assert.commandWorked(sessionDB.getCollection(name).insert({}));

    if (provokeWriteConcernError)
        stopReplicationOnSecondaries(rst);

    const commitResult =
        assert.commandWorkedIgnoringWriteConcernErrors(session.commitTransaction_forTesting());

    jsTestLog(`commitResult ${tojson(commitResult)}`);
    if (provokeWriteConcernError) {
        assertWriteConcernError(commitResult);
    } else {
        assert.commandWorked(commitResult);
    }

    const entries = rst.findOplog(primary,
                                  {
                                      op: "n",
                                      ts: {$gte: commitResult.operationTime},
                                      "o.msg": /.*read-only transaction.*/
                                  },
                                  1)
                        .toArray();

    // If the transaction had a write, it should not *also* do a noop.
    if (shouldWrite) {
        assert.eq(0, entries.length, "shouldn't have written noop oplog entry");
    } else {
        assert.eq(1, entries.length, "should have written noop oplog entry");
    }

    jsTestLog("Ending session");
    session.endSession();
    restartReplSetReplication(rst);
}

for (let readConcernLevel of [null, "local", "majority", "snapshot"]) {
    for (let shouldWrite of [false, true]) {
        for (let provokeWriteConcernError of [false, true]) {
            runTest({
                readConcernLevel: readConcernLevel,
                shouldWrite: shouldWrite,
                provokeWriteConcernError: provokeWriteConcernError
            });
        }
    }
}

rst.stopSet();
}());
