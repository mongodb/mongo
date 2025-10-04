/**
 * Tests that initial sync will abort an attempt if the sync source enters and completes initial
 * sync during cloning (i.e. the source is resynced during an outage).
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = "initial_sync_fails_after_source_resyncs";
const rst = new ReplSetTest({name: testName, nodes: [{}, {rsConfig: {priority: 0, votes: 0}}], useBridge: true});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");
const initialSyncSource = rst.getSecondary();

// Add some data to be cloned.
assert.commandWorked(primaryDb.test.insert([{a: 1}, {b: 2}, {c: 3}]));
rst.awaitReplication();

jsTest.log("Adding the initial sync destination node to the replica set");
const initialSyncNode = rst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        "failpoint.initialSyncHangBeforeCopyingDatabases": tojson({mode: "alwaysOn"}),
        // This test is specifically testing that the cloners detect the source going into initial
        // sync mode, so we turn off the oplog fetcher to ensure that we don't inadvertently test
        // that instead.
        "failpoint.hangBeforeStartingOplogFetcher": tojson({mode: "alwaysOn"}),
        "numInitialSyncAttempts": 1,
        "failpoint.forceSyncSourceCandidate": tojson({mode: "alwaysOn", data: {hostAndPort: initialSyncSource.host}}),
    },
});
rst.reInitiate();
rst.waitForState(initialSyncNode, ReplSetTest.State.STARTUP_2);

// The code handling this case is common to all cloners, so run it only for the stage most likely
// to see an error.
const cloner = "CollectionCloner";
const stage = "query";

// Set us up to hang before finish so we can check status.
const beforeFinishFailPoint = configureFailPoint(initialSyncNode, "initialSyncHangBeforeFinish");
const initialSyncNodeDb = initialSyncNode.getDB("test");
const failPointData = {
    cloner: cloner,
    stage: stage,
    nss: "test.test",
};
// Set us up to stop right before the given stage.
const beforeStageFailPoint = configureFailPoint(initialSyncNodeDb, "hangBeforeClonerStage", failPointData);
// Release the initial failpoint.
assert.commandWorked(
    initialSyncNodeDb.adminCommand({configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}),
);
beforeStageFailPoint.wait();

jsTestLog("Testing resyncing sync source in cloner " + cloner + " stage " + stage);
initialSyncSource.disconnect(initialSyncNode);
rst.restart(initialSyncSource, {startClean: true});
// Wait for the source to reach SECONDARY
rst.awaitSecondaryNodes(undefined, [initialSyncSource]);

jsTestLog("Resuming the initial sync.");
initialSyncSource.reconnect(initialSyncNode);
beforeStageFailPoint.off();
beforeFinishFailPoint.wait();
const res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));

// The initial sync should have failed.
assert.eq(res.initialSyncStatus.failedInitialSyncAttempts, 1);

// Release the initial sync source and sync node oplog fetcher so the test completes.
assert.commandWorked(
    initialSyncNodeDb.adminCommand({configureFailPoint: "hangBeforeStartingOplogFetcher", mode: "off"}),
);
assert.commandWorked(
    initialSyncSource.getDB("admin").adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}),
);

// We need to turn off the above failpoints before this one, otherwise the server may already have
// shutdown before we turn the failpoints off, causing the above commands to fail.
beforeFinishFailPoint.off();

// We want to ensure the initialSyncNode encounters the InitialSyncFailure error and shuts down.
assert.soon(
    () => {
        try {
            initialSyncNodeDb.runCommand({ping: 1});
        } catch (e) {
            return true;
        }
        return false;
    },
    "Node did not shutdown due to initial sync failure",
    ReplSetTest.kDefaultTimeoutMS,
);

// We skip validation and dbhashes because the initial sync failed so the initial sync node is
// invalid and unreachable.
TestData.skipCheckDBHashes = true;
rst.stop(initialSyncNode, null, {skipValidation: true, allowedExitCode: MongoRunner.EXIT_ABRUPT});
rst.stopSet(null, null, {skipValidation: true});
