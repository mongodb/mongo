/**
 * Verify writes inside a transaction are not interpreted as retryable writes in a sharded cluster.
 *
 * @tags: [requires_sharding, uses_transactions]
 */
(function() {
"use strict";

const dbName = "test";
const collName = "foo";
const ns = dbName + '.' + collName;

function runTest(st, session, sessionDB, writeCmdName, writeCmd, isSharded) {
    jsTestLog("Testing " + writeCmdName + ", cmd: " + tojson(writeCmd) + ", sharded: " + isSharded);

    // Fail with retryable error.
    // Sharding tests require failInternalCommands: true, since the mongos appears to mongod to
    // be an internal client.
    const retryableError = ErrorCodes.InterruptedDueToReplStateChange;
    assert.commandWorked(st.rs0.getPrimary().adminCommand({
        configureFailPoint: "failCommand",
        mode: {times: 1},
        data: {errorCode: retryableError, failCommands: [writeCmdName], failInternalCommands: true}
    }));

    session.startTransaction();
    assert.commandFailedWithCode(
        sessionDB.runCommand(writeCmd),
        retryableError,
        "expected write in transaction not to be retried on retryable error, cmd: " +
            tojson(writeCmd) + ", sharded: " + isSharded);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // Fail with closed connection.
    assert.commandWorked(st.rs0.getPrimary().adminCommand({
        configureFailPoint: "failCommand",
        mode: {times: 1},
        data: {closeConnection: true, failCommands: [writeCmdName], failInternalCommands: true}
    }));

    session.startTransaction();
    let res = assert.commandFailed(
        sessionDB.runCommand(writeCmd),
        "expected write in transaction not to be retried on closed connection, cmd: " +
            tojson(writeCmd) + ", sharded: " + isSharded);

    // Network errors during sharded transactions are transient transaction errors, so they're
    // returned as top level codes for all commands, including batch writes.
    assert(ErrorCodes.isNetworkError(res.code), "expected network error, got: " + tojson(res.code));
    assert.eq(res.errorLabels, ["TransientTransactionError"]);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    assert.commandWorked(
        st.rs0.getPrimary().adminCommand({configureFailPoint: "failCommand", mode: "off"}));
}

const kCmdTestCases = [
    {
        name: "insert",
        command: {insert: collName, documents: [{_id: 6}]},
    },
    {
        name: "update",
        command: {update: collName, updates: [{q: {_id: 5}, u: {$set: {x: 1}}}]},
    },
    {
        name: "delete",
        command: {delete: collName, deletes: [{q: {_id: 5}, limit: 1}]},
    },
    {
        name: "findAndModify",  // update
        command: {findAndModify: collName, query: {_id: 5}, update: {$set: {x: 1}}},
    },
    {
        name: "findAndModify",  // delete
        command: {findAndModify: collName, query: {_id: 5}, remove: true},
    }
];

const st = new ShardingTest({shards: 1, config: 1});

const session = st.s.startSession();
const sessionDB = session.getDatabase(dbName);

// Unsharded.
jsTestLog("Testing against unsharded collection");

assert.writeOK(st.s.getDB(dbName)[collName].insert({_id: 0}, {writeConcern: {w: "majority"}}));

kCmdTestCases.forEach(cmdTestCase => {
    runTest(st, session, sessionDB, cmdTestCase.name, cmdTestCase.command, false /*isSharded*/);
});

// Sharded
jsTestLog("Testing against sharded collection");

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.rs0.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: ns}));

kCmdTestCases.forEach(cmdTestCase => {
    runTest(st, session, sessionDB, cmdTestCase.name, cmdTestCase.command, true /*isSharded*/);
});

st.stop();
})();
