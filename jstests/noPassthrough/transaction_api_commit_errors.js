/**
 * Tests that the transaction API handles commit errors correctly.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const kDbName = "testDb";
const kCollName = "testColl";

function makeSingleInsertTxn(doc) {
    return [{
        dbName: kDbName,
        command: {
            insert: kCollName,
            documents: [doc],
        }
    }];
}

function runTxn(conn, commandInfos) {
    return conn.adminCommand({testInternalTransactions: 1, commandInfos: commandInfos});
}

const st = new ShardingTest({config: 1, shards: 1});
const shardPrimary = st.rs0.getPrimary();

// Set up the test collection.
assert.commandWorked(st.s.getDB(kDbName)[kCollName].insert([{_id: 0}]));

//
// Error codes where the API should retry and eventually commit the transaction, either by retrying
// commit until it succeeds or retrying the entire transaction until it succeeds. Fail commands 10
// times to exhaust internal retries at layers below the transaction API.
//

// Retryable error. Note this error is not a NotPrimary error so it won't be rewritten by mongos.
let commitFailPoint =
    configureFailPoint(shardPrimary,
                       "failCommand",
                       {
                           errorCode: ErrorCodes.ReadConcernMajorityNotAvailableYet,
                           failCommands: ["commitTransaction"],
                           failInternalCommands: true,
                       },
                       {times: 10});
let res = assert.commandWorked(runTxn(st.s, makeSingleInsertTxn({_id: 1})));
commitFailPoint.off();

// No command error with a retryable write concern error.
commitFailPoint = configureFailPoint(
    shardPrimary,
    "failCommand",
    {
        writeConcernError:
            {code: NumberInt(ErrorCodes.ReadConcernMajorityNotAvailableYet), errmsg: "foo"},
        failCommands: ["commitTransaction"],
        failInternalCommands: true,
    },
    {times: 10});
res = assert.commandWorked(runTxn(st.s, makeSingleInsertTxn({_id: 2})));
commitFailPoint.off();

//
// Error codes where the API should not retry.
//

// Non-transient commit error with a non-retryable write concern error.
commitFailPoint = configureFailPoint(shardPrimary,
                                     "failCommand",
                                     {
                                         errorCode: ErrorCodes.InternalError,
                                         failCommands: ["commitTransaction"],
                                         failInternalCommands: true,
                                     },
                                     {times: 10});
res = assert.commandFailedWithCode(runTxn(st.s, makeSingleInsertTxn({_id: 3})),
                                   ErrorCodes.InternalError);
commitFailPoint.off();

// No commit error with a non-retryable write concern error.
commitFailPoint = configureFailPoint(
    shardPrimary,
    "failCommand",
    {
        writeConcernError: {code: NumberInt(ErrorCodes.InternalError), errmsg: "foo"},
        failCommands: ["commitTransaction"],
        failInternalCommands: true,
    },
    {times: 10});
// The internal transaction test command will rethrow a write concern error as a top-level error.
res = assert.commandFailedWithCode(runTxn(st.s, makeSingleInsertTxn({_id: 4})),
                                   ErrorCodes.InternalError);
commitFailPoint.off();

// Non-transient commit error that is normally transient. Note NoSuchTransaction is not transient
// with a write concern error, which is what this is meant to simulate. Also note the fail command
// fail point can't take both a write concern error and write concern error so we "cheat" and
// override the error labels.
commitFailPoint = configureFailPoint(shardPrimary,
                                     "failCommand",
                                     {
                                         errorCode: ErrorCodes.NoSuchTransaction,
                                         errorLabels: [],
                                         failCommands: ["commitTransaction"],
                                         failInternalCommands: true,
                                     },
                                     {times: 10});
res = assert.commandFailedWithCode(runTxn(st.s, makeSingleInsertTxn({_id: 5})),
                                   ErrorCodes.NoSuchTransaction);
commitFailPoint.off();

st.stop();
}());
