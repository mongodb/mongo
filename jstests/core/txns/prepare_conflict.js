/**
 * Tests that prepare conflicts for prepared transactions are retried.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: prepareTransaction, profile.
 *   not_allowed_with_signed_security_token,
 *   uses_prepare_transaction,
 *   uses_transactions,
 *   uses_parallel_shell,
 *   requires_profiling,
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";

const dbName = "test";
const collName = "prepare_conflict";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);
const prepareConflictDurationLogMsg = "prepareConflictDuration";

testColl.drop({writeConcern: {w: "majority"}});
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

/**
 * Asserts that a prepare read conflict occurs, and is recorded through the profiler and logs
 * accordingly, by running a find command that uses the provided filter and clusterTime.
 */
const assertPrepareConflict = function assertPrepareReadConflict(filter, clusterTime) {
    assert.commandWorked(testDB.adminCommand({clearLog: "global"}));

    // Uses a 5 second timeout so that there is enough time for the prepared transaction to
    // release its locks and for the command to obtain those locks.
    assert.commandFailedWithCode(
        // Uses afterClusterTime read to make sure that it will block on a prepare conflict.
        testDB.runCommand({
            find: collName,
            filter: filter,
            readConcern: {afterClusterTime: clusterTime},
            maxTimeMS: 5000,
        }),
        ErrorCodes.MaxTimeMSExpired,
    );

    checkLog.contains(testDB, prepareConflictDurationLogMsg);

    let prepareConflicted = false;
    const cur = testDB.system.profile.find({"ns": testColl.getFullName(), "command.filter": filter});
    while (cur.hasNext()) {
        const n = cur.next();
        print("op: " + JSON.stringify(n));
        if (n.prepareReadConflicts > 0) {
            prepareConflicted = true;
        }
    }
    assert(prepareConflicted);
};

// Insert a document modified by the transaction.
const txnDoc = {
    _id: 1,
    x: 1,
};
assert.commandWorked(testColl.insert(txnDoc));
// Insert a document unmodified by the transaction.
const otherDoc = {
    _id: 2,
    y: 2,
};
assert.commandWorked(testColl.insert(otherDoc, {writeConcern: {w: "majority"}}));

// Create an index on 'y' to avoid conflicts on the field.
assert.commandWorked(
    testColl.runCommand({
        createIndexes: collName,
        indexes: [{key: {"y": 1}, name: "y_1"}],
        writeConcern: {w: "majority"},
    }),
);

// Enable the profiler to log slow queries. We expect a 'find' to hang until the prepare
// conflict is resolved.
// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(
    testDB.runCommand({profile: 1, filter: {"command.setFeatureCompatibilityVersion": {"$exists": false}}}),
);

const session = db.getMongo().startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
session.startTransaction({readConcern: {level: "snapshot"}});
assert.commandWorked(
    sessionDB.runCommand({
        update: collName,
        updates: [{q: txnDoc, u: {$inc: {x: 1}}}],
    }),
);
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

// Conflict on _id of prepared document.
assertPrepareConflict({_id: txnDoc._id}, prepareTimestamp);

// Conflict on field that could be added to a prepared document.
assertPrepareConflict({randomField: "random"}, prepareTimestamp);

// No conflict on _id of a non-prepared document.
assert.commandWorked(testDB.adminCommand({clearLog: "global"}));
assert.commandWorked(testDB.runCommand({find: collName, filter: {_id: otherDoc._id}}));
assert.eq(false, checkLog.checkContainsOnce(testDB, prepareConflictDurationLogMsg));

// No conflict on indexed field of a non-prepared document.
assert.commandWorked(testDB.adminCommand({clearLog: "global"}));
assert.commandWorked(testDB.runCommand({find: collName, filter: {y: otherDoc.y}}));
assert.eq(false, checkLog.checkContainsOnce(testDB, prepareConflictDurationLogMsg));

// At this point, we can guarantee all subsequent reads will conflict. Do a read in a parallel
// shell, abort the transaction, then ensure the read succeeded with the old document.
TestData.collName = collName;
TestData.dbName = dbName;
TestData.txnDoc = txnDoc;
const findAwait = startParallelShell(function () {
    const it = db
        .getSiblingDB(TestData.dbName)
        .runCommand({find: TestData.collName, filter: {_id: TestData.txnDoc._id}});
}, db.getMongo().port);
assert.commandWorked(session.abortTransaction_forTesting());

// The find command should be successful.
findAwait({checkExitSuccess: true});

// The document should be unmodified, because we aborted.
assert.eq(txnDoc, testColl.findOne(txnDoc));
