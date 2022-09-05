/**
 * Confirms that aborting an index build on a primaries succeeds despite a concurrent stepDown
 * attempting to interrupt the operation.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ]
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB('test');
const primaryColl = primaryDB.getCollection('test');

assert.commandWorked(primaryColl.insert({a: 1}));

let hangAfterInitFailPoint = configureFailPoint(primaryDB, 'hangAfterInitializingIndexBuild');

jsTestLog("Waiting for index build to start");
const createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {a: 1}, null, [ErrorCodes.IndexBuildAborted]);
IndexBuildTest.waitForIndexBuildToStart(primaryDB, primaryColl.getName(), 'a_1');

jsTestLog("Attempting to abort the index build and blocking before it completes");
let hangBeforeAbortFailPoint = configureFailPoint(primaryDB, 'hangBeforeCompletingAbort');
const abortIndexThread = startParallelShell(() => {
    // We can't assert that this succeeds because it may return an Interrupted error even after it
    // successfully aborts the index build.
    db.getSiblingDB('test').test.dropIndex('a_1');
}, primary.port);
hangBeforeAbortFailPoint.wait();
hangAfterInitFailPoint.off();

jsTestLog("Stepping down the primary");
const stepDown = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({"replSetStepDown": 60, 'force': true}));
}, primary.port);

jsTestLog("Waiting for primary to kill operations");
checkLog.containsJson(primary, 21579);
hangBeforeAbortFailPoint.off();

jsTestLog("Waiting for threads to join");
abortIndexThread();
createIdx();
stepDown();

// Allow primary to step back up.
assert.commandWorked(primaryDB.adminCommand({replSetFreeze: 0}));

rst.awaitReplication();
IndexBuildTest.assertIndexes(primaryColl, 1, ['_id_']);

const secondaryColl = rst.getSecondary().getCollection(primaryColl.getFullName());
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);

rst.stopSet();
})();
