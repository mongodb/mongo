/**
 * Tests that initial sync survives a network error during each stage of the cloning process,
 * except for the query stage.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const testName = "initial_sync_survives_network_error.js";
const rst = new ReplSetTest({name: testName, nodes: 1, useBridge: true});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");
// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Add some data to be cloned.
assert.commandWorked(primaryDb.test.insert([{a: 1}, {b: 2}, {c: 3}]));

jsTest.log("Adding a new node to the replica set");
const secondary = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'}),
        // This test is specifically testing that the cloners stop, so we turn off the
        // oplog fetcher to ensure that we don't inadvertently test that instead.
        'failpoint.hangBeforeStartingOplogFetcher': tojson({mode: 'alwaysOn'}),
        'numInitialSyncAttempts': 1,
    }
});
rst.reInitiate();
rst.waitForState(secondary, ReplSetTest.State.STARTUP_2);

function retryStage(rst, {cloner, stage, extraData}) {
    const nRetries = 3;
    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const secondaryDb = secondary.getDB("test");
    const failPointData = Object.merge(extraData || {}, {cloner: cloner, stage: stage});
    // Set us up to stop right before the given stage.
    const beforeStageFailPoint =
        configureFailPoint(secondaryDb, "hangBeforeClonerStage", failPointData);
    // Set us up to stop after the given stage. This will also release the failpoint for the
    // previous stage, if it was set.
    const afterStageFailPoint =
        configureFailPoint(secondaryDb, "hangAfterClonerStage", failPointData);
    // Release the initial failpoint if set.
    assert.commandWorked(secondaryDb.adminCommand(
        {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));

    beforeStageFailPoint.wait();
    let beforeRetryFailPoint =
        configureFailPoint(secondaryDb, "hangBeforeRetryingClonerStage", failPointData);
    primary.disconnect(secondary);
    jsTestLog("Testing initial attempt in cloner " + cloner + " stage " + stage);
    beforeStageFailPoint.off();
    for (let i = 1; i <= nRetries; i++) {
        jsTestLog("Testing retry #" + i + " in cloner " + cloner + " stage " + stage);
        beforeRetryFailPoint.wait();
        if (i == nRetries) {
            primary.reconnect(secondary);
        }
        // We do this dance with two failpoints because there's no way to skip past a failpoint
        // that we are waiting at and then hit it the next time.
        const beforeRBIDFailPoint = configureFailPoint(
            secondaryDb, "hangBeforeCheckingRollBackIdClonerStage", failPointData);
        beforeRetryFailPoint.off();
        beforeRBIDFailPoint.wait();
        beforeRetryFailPoint =
            configureFailPoint(secondaryDb, "hangBeforeRetryingClonerStage", failPointData);
        beforeRBIDFailPoint.off();
    }
    jsTestLog("Testing retries in cloner " + cloner + " stage " + stage + " finished.");
    afterStageFailPoint.wait();
    jsTestLog("Cloner " + cloner + " stage " + stage + " complete.");
    return afterStageFailPoint;
}
retryStage(rst, {cloner: "AllDatabaseCloner", stage: "connect"});
retryStage(rst, {cloner: "AllDatabaseCloner", stage: "listDatabases"});
retryStage(rst,
           {cloner: "DatabaseCloner", stage: "listCollections", extraData: {database: 'test'}});
retryStage(rst, {cloner: "CollectionCloner", stage: "count", extraData: {nss: 'test.test'}});
const afterStageFailPoint = retryStage(
    rst, {cloner: "CollectionCloner", stage: "listIndexes", extraData: {nss: 'test.test'}});

jsTestLog("Releasing the oplog fetcher failpoint.");
assert.commandWorked(secondary.getDB("test").adminCommand(
    {configureFailPoint: "hangBeforeStartingOplogFetcher", mode: "off"}));

jsTestLog("Releasing the final cloner failpoint.");
afterStageFailPoint.off();
jsTestLog("Waiting for initial sync to complete.");
// Wait for initial sync to complete.
rst.waitForState(secondary, ReplSetTest.State.SECONDARY);
rst.stopSet();
})();
