/**
 * Tests that the auto_retry_on_network_error.js override automatically retries commands on network
 * errors for commands run under a session.
 * @tags: [requires_replication]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

TestData.networkErrorAndTxnOverrideConfig = {
    retryOnNetworkErrors: true,
};

function getThreadName(db) {
    let myUri = db.adminCommand({whatsmyuri: 1}).you;
    return db
        .getSiblingDB("admin")
        .aggregate([{$currentOp: {localOps: true}}, {$match: {client: myUri}}])
        .toArray()[0].desc;
}

function failNextCommand(db, command) {
    let threadName = getThreadName(db);

    assert.commandWorked(
        db.adminCommand({
            configureFailPoint: "failCommand",
            mode: {times: 1},
            data: {
                closeConnection: true,
                failCommands: [command],
                threadName,
            },
        }),
    );
}

const rst = new ReplSetTest({nodes: 1});
rst.startSet();

// awaitLastStableRecoveryTimestamp runs an 'appendOplogNote' command which is not retryable.
rst.initiate(null, "replSetInitiate", {doNotWaitForStableRecoveryTimestamp: true});

// We require the 'setParameter' command to initialize a replica set, and the command will fail
// due to the override below. As a result, we must initiate our replica set before we load these
// files.
await import("jstests/libs/override_methods/network_error_and_txn_override.js");

const dbName = "test";
const collName = "auto_retry";

// The override requires the connection to be run under a session. Use the replica set URL to
// allow automatic re-targeting of the primary on NotWritablePrimary errors.
const db = new Mongo(rst.getURL()).startSession({retryWrites: true}).getDatabase(dbName);

// Commands with no disconnections should work as normal.
assert.commandWorked(db.runCommand({ping: 1}));

// Read commands are automatically retried on network errors.
failNextCommand(db, "find");
assert.commandWorked(db.runCommand({find: collName}));

failNextCommand(db, "find");

// Retryable write commands that can be retried succeed.
failNextCommand(db, "insert");
assert.commandWorked(db[collName].insert({x: 1}));

failNextCommand(db, "insert");
assert.commandWorked(
    db.runCommand({
        insert: collName,
        documents: [{x: 2}, {x: 3}],
        txnNumber: NumberLong(10),
        lsid: {id: UUID()},
    }),
);

// Retryable write commands that cannot be retried (i.e. no transaction number, no session id,
// or are unordered) throw.
failNextCommand(db, "insert");
assert.throws(function () {
    db.runCommand({insert: collName, documents: [{x: 1}, {x: 2}], ordered: false});
});

// The previous command shouldn't have been retried, so run a command to successfully re-target
// the primary, so the connection to it can be closed.
assert.commandWorked(db.runCommand({ping: 1}));

failNextCommand(db, "insert");
assert.throws(function () {
    db.runCommand({insert: collName, documents: [{x: 1}, {x: 2}], ordered: false});
});

// getMore commands can't be retried because we won't know whether the cursor was advanced or
// not.
let cursorId = assert.commandWorked(db.runCommand({find: collName, batchSize: 0})).cursor.id;
failNextCommand(db, "getMore");
assert.throws(function () {
    db.runCommand({getMore: cursorId, collection: collName});
});

cursorId = assert.commandWorked(db.runCommand({find: collName, batchSize: 0})).cursor.id;
failNextCommand(db, "getMore");
assert.throws(function () {
    db.runCommand({getMore: cursorId, collection: collName});
});

rst.stopSet();
