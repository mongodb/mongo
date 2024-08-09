/**
 * Tests that statistics for prepare conflicts are properly tracked in serverStatus.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: prepareTransaction, profile.
 *   not_allowed_with_signed_security_token,
 *   uses_prepare_transaction,
 *   uses_transactions,
 *   requires_snapshot_read,
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = "prepare_conflict_metrics_tracking";
let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testDB = primary.getDB("test");
testDB.dropDatabase();
const testColl = testDB.getCollection(collName);

var basePrepareConflictWaitTime;
var baseNumberOfPrepareConflicts;

// Establishing a baseline makes this test robust against prepare conflicts that may have occurred
// prior to (but not during) it.
const initializeBaseMetrics = () => {
    let opMetrics = testDB.serverStatus().metrics.operation;
    basePrepareConflictWaitTime = opMetrics.prepareConflictWaitMicros;
    baseNumberOfPrepareConflicts = opMetrics.prepareConflicts;
};

const verifyNoPrepareConflictMetricsLogged = function verifyNoPrepareConflictMetricsLogged() {
    let opMetrics = testDB.serverStatus().metrics.operation;
    assert.eq(opMetrics.prepareConflictWaitMicros, basePrepareConflictWaitTime);
    assert.eq(baseNumberOfPrepareConflicts, opMetrics.prepareConflicts);
};

const causePrepareConflictAndVerify = function assertPrepareReadConflict(filter, clusterTime) {
    // Uses a 1 second timeout so that there is enough time for the prepared transaction to
    // release its locks and for the command to obtain those locks.
    assert.commandFailedWithCode(
        // Uses afterClusterTime read to make sure that it will block on a prepare conflict.
        testDB.runCommand({
            find: collName,
            filter: filter,
            readConcern: {afterClusterTime: clusterTime},
            maxTimeMS: 1000
        }),
        ErrorCodes.MaxTimeMSExpired);

    let opMetrics = testDB.serverStatus().metrics.operation;
    assert.gt(opMetrics.prepareConflictWaitMicros, basePrepareConflictWaitTime);
    assert.eq(++baseNumberOfPrepareConflicts, opMetrics.prepareConflicts);
    basePrepareConflictWaitTime = opMetrics.prepareConflictWaitMicros;
};

testColl.drop({writeConcern: {w: "majority"}});

// Insert a document modified by the transaction.
const txnDoc = {
    _id: 1,
    x: 1
};
assert.commandWorked(testColl.insert(txnDoc));
// Insert a document unmodified by the transaction.
const otherDoc = {
    _id: 2,
    y: 2
};
assert.commandWorked(testColl.insert(otherDoc, {writeConcern: {w: "majority"}}));

// Create an index on 'y' to avoid conflicts on the field.
assert.commandWorked(testColl.runCommand({
    createIndexes: collName,
    indexes: [{key: {"y": 1}, name: "y_1"}],
    writeConcern: {w: "majority"}
}));

const session = testDB.getMongo().startSession();
const sessionDB = session.getDatabase(dbName);
session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandWorked(sessionDB.runCommand({
    update: collName,
    updates: [{q: txnDoc, u: {$inc: {x: 1}}}],
}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

// Setup and ensure tracking of 2 prepare conflicts.
initializeBaseMetrics();
causePrepareConflictAndVerify({_id: txnDoc._id}, prepareTimestamp);
causePrepareConflictAndVerify({randomField: "random"}, prepareTimestamp);

// Abort the transaction and ensure that the same find now succeeds.
assert.commandWorked(session.abortTransaction_forTesting());
assert.commandWorked(testDB.runCommand({find: collName, filter: {_id: txnDoc._id}}));
verifyNoPrepareConflictMetricsLogged();
rst.stopSet();
