/**
 * Ensures that index builds encountering a DataCorruptionDetected error log and increment a metric.
 *
 * @tags: [
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = 'test';
const collName = 'coll';
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

assert.commandWorked(primaryColl.insert({a: 1}));

rst.awaitReplication();

const hangAfterInitializingIndexBuild =
    configureFailPoint(primary, "hangAfterInitializingIndexBuild");
const createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {a: 1}, null, [ErrorCodes.DataCorruptionDetected]);

hangAfterInitializingIndexBuild.wait();
const WTRecordStoreUassertOutOfOrder =
    configureFailPoint(primary, "WTRecordStoreUassertOutOfOrder");
const hangBeforeAbort =
    configureFailPoint(primary, "hangIndexBuildBeforeTransitioningReplStateTokAwaitPrimaryAbort");
hangAfterInitializingIndexBuild.off();

hangBeforeAbort.wait();
// Index build: data corruption detected.
checkLog.containsJson(primary, 7333600);
assert.eq(1, primaryDB.serverStatus().indexBuilds.failedDueToDataCorruption);

// Disable out-of-order failpoint so clean-up can succeed.
WTRecordStoreUassertOutOfOrder.off();
hangBeforeAbort.off();

jsTestLog("Waiting for threads to join");
createIdx();

// Check server status metric.

rst.awaitReplication();
IndexBuildTest.assertIndexes(primaryColl, 1, ['_id_']);

rst.stopSet();
})();
