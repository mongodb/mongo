// Test TransientTransactionError error label for commands in transactions with write concern.
// @tags: [uses_transactions]
(function() {
"use strict";

load("jstests/libs/check_log.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/rslib.js");

const dbName = "test";
const collName = "transient_txn_error_labels_with_write_concern";

// We are testing coordinateCommitTransaction, which requires the nodes to be started with
// --shardsvr.
const st = new ShardingTest(
    {config: 1, mongos: 1, shards: {rs0: {nodes: [{}, {rsConfig: {priority: 0}}]}}});
const rst = st.rs0;

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
assert.eq(primary, rst.nodes[0]);
const testDB = primary.getDB(dbName);

const sessionOptions = {
    causalConsistency: false
};
const writeConcernMajority = {
    w: "majority",
    wtimeout: 500
};

assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

jsTest.log("Write concern errors should not have error labels");
// Start a new session on the primary.
let session = primary.startSession(sessionOptions);
let sessionDb = session.getDatabase(dbName);
let sessionColl = sessionDb.getCollection(collName);
stopServerReplication(rst.getSecondaries());
session.startTransaction({writeConcern: writeConcernMajority});
assert.commandWorked(sessionColl.insert({_id: "write-with-write-concern"}));
let res = session.commitTransaction_forTesting();
checkWriteConcernTimedOut(res);
assert(!res.hasOwnProperty("code"));
assert(!res.hasOwnProperty("errorLabels"));
restartServerReplication(rst.getSecondaries());

function runNoSuchTransactionTests(cmd, cmdName) {
    jsTest.log("Running NoSuchTransaction tests for " + cmdName);
    assert.commandWorked(primary.adminCommand({clearLog: "global"}));

    jsTest.log(cmdName + " should wait for write concern even if it returns NoSuchTransaction");
    rst.awaitReplication();
    stopServerReplication(rst.getSecondaries());
    // Use a txnNumber that is one higher than the server has tracked.
    res = sessionDb.adminCommand(Object.assign(Object.assign({}, cmd), {
        txnNumber: NumberLong(session.getTxnNumber_forTesting() + 1),
        autocommit: false,
        writeConcern: writeConcernMajority
    }));
    checkWriteConcernTimedOut(res);
    assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);

    jsTest.log("NoSuchTransaction with write concern error is not transient");
    assert(!res.hasOwnProperty("errorLabels"));

    jsTest.log("NoSuchTransaction without write concern error is transient");
    restartServerReplication(rst.getSecondaries());
    // Use a txnNumber that is one higher than the server has tracked.
    res = sessionDb.adminCommand(Object.assign(Object.assign({}, cmd), {
        txnNumber: NumberLong(session.getTxnNumber_forTesting() + 1),
        autocommit: false,
        writeConcern: {w: "majority"}  // Wait with a long timeout.
    }));
    assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
    assert(!res.hasOwnProperty("writeConcernError"), res);
    assert.eq(res["errorLabels"], ["TransientTransactionError"], res);

    jsTest.log("If the noop write for NoSuchTransaction cannot occur, the error is not transient");

    // Lock 'local' database in X mode.
    let lockShell = startParallelShell(function() {
        assert.commandFailed(db.adminCommand({
            sleep: 1,
            secs: 500,
            lock: "w",
            lockTarget: "local",
            $comment: "transient_txn_error_labels_with_write_concern lock sleep"
        }));
    }, rst.ports[0]);

    // Wait for sleep to appear in currentOp
    let opId = -1;
    assert.soon(function() {
        const curopRes = testDB.currentOp();
        assert.commandWorked(curopRes);
        const foundOp = curopRes["inprog"].filter(
            op => (op["ns"] == "admin.$cmd" &&
                   op["command"]["$comment"] ==
                       "transient_txn_error_labels_with_write_concern lock sleep"));
        if (foundOp.length == 1) {
            opId = foundOp[0]["opid"];
        }
        return (foundOp.length == 1);
    });

    // The server will attempt to perform a noop write, since the command returns
    // NoSuchTransaction. The noop write will time out acquiring a lock on the 'local' database.
    // This should not be a TransientTransactionError, since the server has not successfully
    // replicated a write to confirm that it is primary.
    // Use a txnNumber that is one higher than the server has tracked.
    res = sessionDb.adminCommand(Object.assign(Object.assign({}, cmd), {
        txnNumber: NumberLong(session.getTxnNumber_forTesting() + 1),
        autocommit: false,
        writeConcern: writeConcernMajority,
        maxTimeMS: 1000
    }));
    assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);
    assert(!res.hasOwnProperty("errorLabels"));

    assert.commandWorked(testDB.killOp(opId));
    lockShell();

    rst.awaitReplication();
}

runNoSuchTransactionTests({commitTransaction: 1}, "commitTransaction");

runNoSuchTransactionTests({coordinateCommitTransaction: 1, participants: []},
                          "coordinateCommitTransaction");

session.endSession();

st.stop();
}());
