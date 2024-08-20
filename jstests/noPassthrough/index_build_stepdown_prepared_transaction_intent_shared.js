/**
 * Tests that we don't hit a 4-way deadlock between an index builder, prepared transaction, step
 * down and an operation taking MODE_IS collection lock.
 *
 * This tests the following scenario:
 * 1) Starts and index build.
 * 2) Prepares a transaction which holds the collection lock in IX mode.
 * 3) Waits for the index build to attempt to acquire the collection lock in X mode to commit, but
 *    blocks behind the prepared transaction due to a collection lock conflict.
 * 4) Starts an operation which takes a MODE_IS collection lock (and RSTL in MODE_IX), and is not
 *    killed by step down.
 * 5) Steps down the primary, which enqueues the RSTL in X mode.
 * 6) Ensures the index build does not block waiting indefinitely for collection MODE_X lock, and is
 *    not blocking the operation taking collection lock in MODE_IS, which in turn would block
 *    stepdown by holding the RSTL in MODE_IX.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";
import {waitForState} from "jstests/replsets/rslib.js";

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

// Populate collection to trigger an index build.
assert.commandWorked(primaryColl.insert({x: 1}));

// Enable fail point which makes hybrid index build hang before taking locks for commit.
const hangBeforeCommit = configureFailPoint(primaryDB, "hangIndexBuildBeforeCommit");

const indexName = 'myidx';
const indexThread = IndexBuildTest.startIndexBuild(primary,
                                                   primaryColl.getFullName(),
                                                   {x: 1},
                                                   {name: indexName},
                                                   ErrorCodes.InterruptedDueToReplStateChange);

// Get opId and ensure it is the one for the above index build.
const filter = {
    "desc": {$regex: /IndexBuildsCoordinatorMongod-*/}
};
const opId = IndexBuildTest.waitForIndexBuildToStart(primaryDB, collName, indexName, filter);

jsTestLog("Waiting for index build to hit failpoint");
hangBeforeCommit.wait();

jsTestLog("Start txn");
const session = primary.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);
session.startTransaction();
assert.commandWorked(sessionColl.insert({x: 1}, {$set: {y: 1}}));

jsTestLog("Prepare txn");
PrepareHelpers.prepareTransaction(session);

// Wait the index build to enqueue MODE_X collection lock.
hangBeforeCommit.off();
assert.soonNoExcept(() => {
    const result = primary.getDB("admin")
                       .aggregate([{$currentOp: {idleConnections: true}}, {$match: filter}])
                       .toArray();
    assert(result.length === 1, tojson(result));
    return result[0].waitingForLock;
});

jsTestLog("Run operation taking MODE_IS lock");
const otherCommandThread =
    startParallelShell(funWithArgs(function(dbName, collName) {
                           assert.commandWorked(db.runCommand({
                               validateDBMetadata: 1,
                               db: dbName,
                               collection: collName,
                               apiParameters: {version: "1", strict: true}
                           }));
                       }, primaryDB.getName(), primaryColl.getName()), primary.port);

const curopValidateDb = () => {
    const filterValidateDB = {"command.validateDBMetadata": 1};
    return primary.getDB("admin")
        .aggregate([{$currentOp: {idleConnections: true}}, {$match: filterValidateDB}])
        .toArray();
};

// Wait for the above operation to block trying to acquire MODE_IS lock, or to succeed. The first
// part is required to reproduce the deadlock, but if the deadlock is fixed the operation is
// expected to succeed.
try {
    assert.soonNoExcept(() => {
        const result = curopValidateDb();
        assert(result.length === 1, tojson(result));
        return result[0].waitingForLock;
    }, "Failed to wait for validateDBMetadata to take locks", 10000, {runHangAnalyzer: false});
} catch (e) {
    // If the deadlock is fixed, we don't expect to find the operation, as it will have acquired
    // locks and completed.
    const result = curopValidateDb();
    assert(result.length === 0, "Expected validateDBMetadata operation to have succeeded.");
}

let newPrimary = rst.getSecondary();

// If there is a deadlock, the stepdown command is expected to timeout.
jsTestLog("Make primary step down");
const stepDownThread = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({"replSetStepDown": 60, force: true}));
}, primary.port);

// Wait for threads to join.
indexThread();
stepDownThread();
otherCommandThread();

jsTestLog("Stepping-up new primary");
rst.stepUp(newPrimary);
waitForState(newPrimary, ReplSetTest.State.PRIMARY);

jsTestLog("Aborting transaction and waiting for index build to finish");
const newSession = new _DelegatingDriverSession(newPrimary, session);
assert.commandWorked(newSession.abortTransaction_forTesting());

IndexBuildTest.waitForIndexBuildToStop(newPrimary.getDB(dbName), collName, indexName);
IndexBuildTest.waitForIndexBuildToStop(primary.getDB(dbName), collName, indexName);
rst.awaitReplication();

IndexBuildTest.assertIndexes(
    newPrimary.getDB(dbName).getCollection(collName), 2, ["_id_", indexName], []);
IndexBuildTest.assertIndexes(primaryColl, 2, ["_id_", indexName], []);

rst.stopSet();
