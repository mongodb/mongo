/**
 * Tests that initial sync survives a restart during each stage of the cloning process.
 * @tags: [
 *   requires_persistence,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const testName = "initial_sync_survives_restart";
const rst = new ReplSetTest({name: testName, nodes: 1});
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
    const nRetries = 2;
    let primary = rst.getPrimary();
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

    jsTestLog("Testing restart of sync source in cloner " + cloner + " stage " + stage);

    // We stop the node and wait for it, then start it separately, to avoid the clone completing
    // before the node actually stops.
    rst.stop(primary, null, null, {forRestart: true, waitPid: true});

    // Release the syncing node fail point to allow retries while the node is down and restarting.
    beforeStageFailPoint.off();

    // Make sure some retries happen while the sync source is completely down.
    let beforeRetryFailPoint = configureFailPoint(
        secondaryDb, "hangBeforeRetryingClonerStage", failPointData, {skip: nRetries});
    beforeRetryFailPoint.wait();
    beforeRetryFailPoint.off();

    // Turning on rsSyncApplyStop prevents the sync source from coming out of RECOVERING,
    // so we can ensure the syncing node does some retries while the sync source is not ready.
    const options = {
        setParameter: {'failpoint.rsSyncApplyStop': tojson({mode: 'alwaysOn'})},
        waitForConnect: true
    };
    primary = rst.start(primary, options, true /* restart */);

    // Wait for the sync source to be in RECOVERING.
    assert.commandWorked(primary.adminCommand({
        waitForFailPoint: "rsSyncApplyStop",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    // Make sure some retries happen while the sync source is available and in "RECOVERING"
    beforeRetryFailPoint = configureFailPoint(
        secondaryDb, "hangBeforeRetryingClonerStage", failPointData, {skip: nRetries});
    beforeRetryFailPoint.wait();
    beforeRetryFailPoint.off();

    // Now let the sync source finish recovering and keep retrying.
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}));
    afterStageFailPoint.wait();
    jsTestLog("Cloner " + cloner + " stage " + stage + " complete.");
    return afterStageFailPoint;
}
retryStage(rst, {cloner: "AllDatabaseCloner", stage: "connect"});
retryStage(rst, {cloner: "AllDatabaseCloner", stage: "listDatabases"});
retryStage(rst,
           {cloner: "DatabaseCloner", stage: "listCollections", extraData: {database: 'test'}});
retryStage(rst, {cloner: "CollectionCloner", stage: "count", extraData: {nss: 'test.test'}});
retryStage(rst, {cloner: "CollectionCloner", stage: "listIndexes", extraData: {nss: 'test.test'}});
const afterStageFailPoint =
    retryStage(rst, {cloner: "CollectionCloner", stage: "query", extraData: {nss: 'test.test'}});

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
