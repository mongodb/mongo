/**
 * Tests that we can resume cloning a collection when a network error occurs in the middle
 * of that process. This also tests that we correctly clean up old cursors on the sync source.
 * Scenarios included:
 *  - failure in first batch
 *  - failure in subsequent batches
 *  - multiple resumable failures during the same clone
 */
(function() {
"use strict";

load("jstests/replsets/rslib.js");  // For setLogVerbosity()
load("jstests/libs/fail_point_util.js");

// Verify the 'find' command received by the primary includes a resume token request.
function checkHasRequestResumeToken() {
    checkLog.contains(primary, /\$_requestResumeToken"?: ?true/);
}

// Verify the 'find' command received by the primary has no resumeAfter (yet).
function checkNoResumeAfter() {
    assert.throws(function() {
        checkLog.contains(primary, /\$_resumeAfter/, 3 * 1000);
    });
}

// Verify the 'find' command received by the primary has resumeAfter set with the given recordId.
function checkHasResumeAfter(recordId) {
    checkLog.contains(primary, `"$_resumeAfter":{"$recordId":${recordId}}`);
}

const beforeRetryFailPointName = "hangBeforeRetryingClonerStage";
const afterBatchFailPointName = "initialSyncHangCollectionClonerAfterHandlingBatchResponse";

const testName = "collection_cloner_resume_after_network_error";
const rst = new ReplSetTest({name: testName, nodes: 1, useBridge: true});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Add some data to be cloned.
assert.commandWorked(primaryDb.test.insert([
    /* first network error here */
    {a: 1},
    {b: 2}, /* batch 1 - second network error here */
    {c: 3},
    {d: 4}, /* batch 2 - third network error here */
    {e: 5},
    {f: 6},
    {g: 7} /* batches 3 and 4, finish cloning */
]));

jsTest.log("Adding a new node to the replica set");
const secondary = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'}),
        // Hang right after cloning the last document.
        'failpoint.initialSyncHangDuringCollectionClone':
            tojson({mode: 'alwaysOn', data: {namespace: "test.test", numDocsToClone: 7}}),
        // This test is specifically testing that the cloners stop, so we turn off the
        // oplog fetcher to ensure that we don't inadvertently test that instead.
        'failpoint.hangBeforeStartingOplogFetcher': tojson({mode: 'alwaysOn'}),
        'collectionClonerBatchSize': 2,
        'numInitialSyncAttempts': 1,
    }
});
rst.reInitiate();
rst.waitForState(secondary, ReplSetTest.State.STARTUP_2);

// Enable command logging on the primary so we can verify what commands we send over.
assert.commandWorked(primary.adminCommand(
    {"setParameter": 1, "logComponentVerbosity": {"command": {"verbosity": 1}}}));

jsTestLog("Beginning tests for collection cloner stage query");

// Preliminary setup for some of our failpoints.
const secondaryDb = secondary.getDB("test");
const failPointData = {
    cloner: "CollectionCloner",
    stage: "query",
    nss: "test.test"
};
const beforeStageFailPoint =
    configureFailPoint(secondaryDb, "hangBeforeClonerStage", failPointData);
let beforeRetryFailPoint = configureFailPoint(secondaryDb, beforeRetryFailPointName, failPointData);

// Release the initial failpoint. We won't be needing it anymore.
assert.commandWorked(secondaryDb.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));

// We are still waiting to begin the query stage.
beforeStageFailPoint.wait();

/**
 * First network error.
 *
 * The clone won't have anywhere to resume from, so the query will be retried.
 */
jsTestLog("Now testing: network error before first batch");

// Disconnect the nodes so we get a network error on the very first batch.
primary.disconnect(secondary);
beforeStageFailPoint.off();
beforeRetryFailPoint.wait();
checkHasRequestResumeToken();  // check params of first query
checkNoResumeAfter();
assert.commandWorked(secondaryDb.adminCommand({clearLog: "global"}));

// Reconnect the nodes and let the retry commence.
primary.reconnect(secondary);
let afterBatchFailPoint = configureFailPoint(secondaryDb, afterBatchFailPointName);
beforeRetryFailPoint.off();
afterBatchFailPoint.wait();
checkHasRequestResumeToken();  // check params of first query retry
checkNoResumeAfter();
assert.commandWorked(secondaryDb.adminCommand({clearLog: "global"}));

/**
 * Second network error.
 *
 * The clone is one batch in already, so it will have a resume token to use.
 */
jsTestLog("Now testing: network error after first batch (first resume)");

// Disconnect the nodes so we get a network error on the next batch.
primary.disconnect(secondary);
beforeRetryFailPoint = configureFailPoint(secondaryDb, beforeRetryFailPointName, failPointData);
afterBatchFailPoint.off();
beforeRetryFailPoint.wait();

// Reconnect the nodes and let the resume commence.
primary.reconnect(secondary);
afterBatchFailPoint = configureFailPoint(secondaryDb, afterBatchFailPointName);
beforeRetryFailPoint.off();
afterBatchFailPoint.wait();
checkHasRequestResumeToken();  // check params of first query resume
checkHasResumeAfter(2 /* recordId */);
checkHasRequestResumeToken();
assert.commandWorked(secondaryDb.adminCommand({clearLog: "global"}));

/**
 * Third (and last) network error.
 *
 * The clone is two batches in already, so it will have a new resume token to use.
 */
jsTestLog("Now testing: network error after first batch (first resume)");

// Disconnect the nodes so we get a network error on the next batch.
primary.disconnect(secondary);
beforeRetryFailPoint = configureFailPoint(secondaryDb, beforeRetryFailPointName, failPointData);
afterBatchFailPoint.off();
beforeRetryFailPoint.wait();

// Reconnect the nodes and let the resume commence.
primary.reconnect(secondary);
afterBatchFailPoint = configureFailPoint(secondaryDb, afterBatchFailPointName);
beforeRetryFailPoint.off();
afterBatchFailPoint.wait();
checkHasRequestResumeToken();  // check params of second query resume
checkHasResumeAfter(4 /* recordId */);
assert.commandWorked(secondaryDb.adminCommand({clearLog: "global"}));

// Reconnect nodes and turn off cloner failpoints.
primary.reconnect(secondary);
beforeRetryFailPoint.off();
afterBatchFailPoint.off();

// Make sure we have cloned all documents.
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: "initialSyncHangDuringCollectionClone",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Make sure we have cloned exactly four batches and have never requested the same batch more
// than once.
const res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1, initialSync: 1}));
assert(res.initialSyncStatus,
       () => "Response should have an 'initialSyncStatus' field: " + tojson(res));
assert.eq(res.initialSyncStatus.databases.test["test.test"].fetchedBatches, 4);

// Release the last cloner failpoint.
assert.commandWorked(secondaryDb.adminCommand(
    {configureFailPoint: "initialSyncHangDuringCollectionClone", mode: "off"}));

// Also let the oplog fetcher run so we can complete initial sync.
jsTestLog("Releasing the oplog fetcher failpoint.");
assert.commandWorked(
    secondaryDb.adminCommand({configureFailPoint: "hangBeforeStartingOplogFetcher", mode: "off"}));

jsTestLog("Waiting for initial sync to complete.");
rst.waitForState(secondary, ReplSetTest.State.SECONDARY);

rst.stopSet();
})();
