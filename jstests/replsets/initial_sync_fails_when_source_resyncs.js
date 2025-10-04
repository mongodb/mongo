/**
 * Tests that initial sync will abort an attempt if the sync source enters initial sync during
 * cloning. This test will timeout if the attempt is not aborted.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const testName = "initial_sync_fails_when_source_resyncs";
const rst = new ReplSetTest({name: testName, nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB("test");
let initialSyncSource = rst.getSecondary();

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

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

// We hold the source in initial sync mode.
initialSyncSource = rst.restart(initialSyncSource, {
    startClean: true,
    setParameter: {"failpoint.initialSyncHangBeforeFinish": tojson({mode: "alwaysOn"})},
});
// Wait for the source to go into initial sync.
rst.waitForState(initialSyncSource, ReplSetTest.State.STARTUP_2);

jsTestLog("Resuming the initial sync.");
beforeStageFailPoint.off();
beforeFinishFailPoint.wait();
const res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));

// The initial sync should have failed.
assert.eq(res.initialSyncStatus.failedInitialSyncAttempts, 1);
// Release the sync node oplog fetcher so the test completes.
assert.commandWorked(
    initialSyncNodeDb.adminCommand({configureFailPoint: "hangBeforeStartingOplogFetcher", mode: "off"}),
);
beforeFinishFailPoint.off();

// Release the initial sync source so the test completes.
assert.commandWorked(
    initialSyncSource.getDB("admin").adminCommand({configureFailPoint: "initialSyncHangBeforeFinish", mode: "off"}),
);

// Wait for the fassert to stop the initial sync node.
assert.eq(MongoRunner.EXIT_ABRUPT, waitMongoProgram(initialSyncNode.port));

// We skip validation and dbhashes because the initial sync failed so the initial sync node is
// invalid and unreachable.
TestData.skipCheckDBHashes = true;
rst.stopSet(null, null, {skipValidation: true});
