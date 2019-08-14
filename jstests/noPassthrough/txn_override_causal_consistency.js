/**
 * Verifies the network_error_and_txn_override passthrough respects the causal consistency setting
 * on TestData when starting a transaction.
 *
 * @tags: [requires_replication, uses_transactions]
 */
(function() {
"use strict";

const dbName = "test";
const collName = "foo";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const conn = new Mongo(rst.getPrimary().host);

// Create the collection so the override doesn't try to when it is not expected.
assert.commandWorked(conn.getDB(dbName).createCollection(collName));

// Override runCommand to add each command it sees to a global array that can be inspected by
// this test and to allow mocking certain responses.
let cmdObjsSeen = [];
let mockNetworkError, mockFirstResponse, mockFirstCommitResponse;
const mongoRunCommandOriginal = Mongo.prototype.runCommand;
Mongo.prototype.runCommand = function runCommandSpy(dbName, cmdObj, options) {
    cmdObjsSeen.push(cmdObj);

    if (mockNetworkError) {
        mockNetworkError = undefined;
        throw new Error("network error");
    }

    if (mockFirstResponse) {
        const mockedRes = mockFirstResponse;
        mockFirstResponse = undefined;
        return mockedRes;
    }

    const cmdName = Object.keys(cmdObj)[0];
    if (cmdName === "commitTransaction" && mockFirstCommitResponse) {
        const mockedRes = mockFirstCommitResponse;
        mockFirstCommitResponse = undefined;
        return mockedRes;
    }

    return mongoRunCommandOriginal.apply(this, arguments);
};

// Runs the given function with a collection from a session made with the sessionOptions on
// TestData and asserts the seen commands that would start a transaction have or do not have
// afterClusterTime.
function inspectFirstCommandForAfterClusterTime(conn, cmdName, isCausal, expectRetry, func) {
    const session = conn.startSession(TestData.sessionOptions);
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB[collName];

    cmdObjsSeen = [];
    func(sessionColl);

    // Find all requests sent with the expected command name, in case the scenario allows
    // retrying more than once or expects to end with a commit.
    let cmds = [];
    if (!expectRetry) {
        assert.eq(1, cmdObjsSeen.length);
        cmds.push(cmdObjsSeen[0]);
    } else {
        assert.lt(1, cmdObjsSeen.length);
        cmds = cmdObjsSeen.filter(obj => Object.keys(obj)[0] === cmdName);
    }

    for (let cmd of cmds) {
        if (isCausal) {
            assert(cmd.hasOwnProperty("$clusterTime"),
                   "Expected " + tojson(cmd) + " to have a $clusterTime.");
            assert(cmd.hasOwnProperty("readConcern"),
                   "Expected " + tojson(cmd) + " to have a read concern.");
            assert(cmd.readConcern.hasOwnProperty("afterClusterTime"),
                   "Expected " + tojson(cmd) + " to have an afterClusterTime.");
        } else {
            if (TestData.hasOwnProperty("enableMajorityReadConcern") &&
                TestData.enableMajorityReadConcern === false) {
                // Commands not allowed in a transaction without causal consistency will not
                // have a read concern on variants that don't enable majority read concern.
                continue;
            }

            assert(cmd.hasOwnProperty("readConcern"),
                   "Expected " + tojson(cmd) + " to have a read concern.");
            assert(!cmd.readConcern.hasOwnProperty("afterClusterTime"),
                   "Expected " + tojson(cmd) + " to not have an afterClusterTime.");
        }
    }

    // Run a command not runnable in a transaction to reset the override's transaction state.
    assert.commandWorked(sessionDB.runCommand({ping: 1}));

    session.endSession();
}

// Helper methods for testing specific commands.

function testInsert(conn, isCausal, expectRetry) {
    inspectFirstCommandForAfterClusterTime(conn, "insert", isCausal, expectRetry, (coll) => {
        assert.commandWorked(coll.insert({x: 1}));
    });
}

function testFind(conn, isCausal, expectRetry) {
    inspectFirstCommandForAfterClusterTime(conn, "find", isCausal, expectRetry, (coll) => {
        assert.eq(0, coll.find({y: 1}).itcount());
    });
}

function testCount(conn, isCausal, expectRetry) {
    inspectFirstCommandForAfterClusterTime(conn, "count", isCausal, expectRetry, (coll) => {
        assert.eq(0, coll.count({y: 1}));
    });
}

function testCommit(conn, isCausal, expectRetry) {
    inspectFirstCommandForAfterClusterTime(conn, "find", isCausal, expectRetry, (coll) => {
        assert.eq(0, coll.find({y: 1}).itcount());
        assert.commandWorked(coll.getDB().runCommand({ping: 1}));  // commits the transaction.
    });
}

// Load the txn_override after creating the spy, so the spy will see commands after being
// transformed by the override. Also configure network error retries because several suites use
// both.
TestData.networkErrorAndTxnOverrideConfig = {
    wrapCRUDinTransactions: true,
    retryOnNetworkErrors: true
};
load("jstests/libs/override_methods/network_error_and_txn_override.js");

TestData.logRetryAttempts = true;

// Run a command to guarantee operation time is initialized on the database's session.
assert.commandWorked(conn.getDB(dbName).runCommand({ping: 1}));

function runTest() {
    for (let isCausal of [false, true]) {
        jsTestLog("Testing with isCausal = " + isCausal);
        TestData.sessionOptions = {causalConsistency: isCausal};

        // Commands that accept read and write concern allowed in a transaction.
        testInsert(conn, isCausal, false /*expectRetry*/);
        testFind(conn, isCausal, false /*expectRetry*/);

        // Command that can accept read concern not allowed in a transaction.
        testCount(conn, isCausal, false /*expectRetry*/);

        // Command that attempts to implicitly create a collection.
        conn.getDB(dbName)[collName].drop();
        testInsert(conn, isCausal, true /*expectRetry*/);

        // Command that can accept read concern with retryable error.
        mockFirstResponse = {ok: 0, code: ErrorCodes.CursorKilled};
        testFind(conn, isCausal, true /*expectRetry*/);

        // Commands that can accept read and write concern with network error.
        mockNetworkError = true;
        testInsert(conn, isCausal, true /*expectRetry*/);

        mockNetworkError = true;
        testFind(conn, isCausal, true /*expectRetry*/);

        // Command that can accept read concern not allowed in a transaction with network error.
        mockNetworkError = true;
        testCount(conn, isCausal, true /*expectRetry*/);

        // Commands that can accept read and write concern with transient transaction error.
        mockFirstResponse = {
            ok: 0,
            code: ErrorCodes.NoSuchTransaction,
            errorLabels: ["TransientTransactionError"]
        };
        testFind(conn, isCausal, true /*expectRetry*/);

        mockFirstResponse = {
            ok: 0,
            code: ErrorCodes.NoSuchTransaction,
            errorLabels: ["TransientTransactionError"]
        };
        testInsert(conn, isCausal, true /*expectRetry*/);

        // Transient transaction error on commit attempt.
        mockFirstCommitResponse = {
            ok: 0,
            code: ErrorCodes.NoSuchTransaction,
            errorLabels: ["TransientTransactionError"]
        };
        testCommit(conn, isCausal, true /*expectRetry*/);

        // Network error on commit attempt.
        mockFirstCommitResponse = {ok: 0, code: ErrorCodes.NotMaster};
        testCommit(conn, isCausal, true /*expectRetry*/);
    }
}

runTest();

// With read concern majority disabled.
TestData.enableMajorityReadConcern = false;
runTest();
delete TestData.enableMajorityReadConcern;

rst.stopSet();
})();
