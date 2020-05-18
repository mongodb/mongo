/*
 * Tests that nodes in the same region as the primary eventually do not sync across data centers. We
 * do this with a three-node replica set (P, S1, and S2). P and S1 are in the same data center,
 * while S2 is in its own data center. Initially, S1 syncs from S2, and S2 syncs from P. We verify
 * that eventually S1 will decide to sync from P, because it is in the same datacenter as P and thus
 * has a lower ping time.
 *
 * @tags: [requires_fcv_46]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/libs/sync_source.js");

const name = jsTestName();
const rst = new ReplSetTest({
    name,
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    nodeOptions: {
        setParameter: {
            // Set 'maxNumSyncSourceChangesPerHour' to a high value to remove the limit on how many
            // times nodes change sync sources in an hour.
            maxNumSyncSourceChangesPerHour: 99,
        }
    },
    settings: {
        // Set the heartbeat interval to a low value to reduce the amount of time spent waiting for
        // a heartbeat from sync source candidates.
        heartbeatIntervalMillis: 250,
    },
    useBridge: true
});

rst.startSet();
rst.initiateWithHighElectionTimeout();
rst.awaitReplication();

const primary = rst.getPrimary();
const testNode = rst.getSecondaries()[0];
const secondary = rst.getSecondaries()[1];

const primaryDB = primary.getDB(name);
const primaryColl = primaryDB["testColl"];

assert.commandWorked(primaryColl.insert({"steady": "state"}, {writeConcern: {w: 3}}));

// Verify we haven't changed sync sources due to finding a significantly closer node yet.
let serverStatus = assert.commandWorked(testNode.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(0, serverStatus.syncSource.numSyncSourceChangesDueToSignificantlyCloserNode);

jsTestLog("Forcing sync sources for the secondaries");
const secondaryForceSyncSource = forceSyncSource(rst, secondary, primary);
const testNodeForceSyncSource = forceSyncSource(rst, testNode, secondary);

// Partition the nodes so that 'testNode' is in the same data center as the primary,
const westDC = new DataCenter("westDC", [primary, testNode]);
const eastDC = new DataCenter("eastDC", [secondary]);
delayMessagesBetweenDataCenters(westDC, eastDC, 50 /* delayMillis */);

// Hang 'testNode' in the oplog fetcher to ensure that sync source candidates are ahead of us.
const hangOplogFetcherBeforeAdvancingLastFetched =
    configureFailPoint(testNode, "hangOplogFetcherBeforeAdvancingLastFetched");

// Do a write to reduce the time spent waiting for a batch.
assert.commandWorked(primaryColl.insert({"make": "batch"}, {writeConcern: {w: 2}}));

hangOplogFetcherBeforeAdvancingLastFetched.wait();

secondaryForceSyncSource.off();
testNodeForceSyncSource.off();

// Ensure that 'testNode' is set up to change sync sources to the primary before unpausing the
// oplog fetcher. We do this by advancing the optime on the primary, then waiting for the node to
// receive a heartbeat with the advanced timestamp from the primary. We also wait to receive a
// heartbeat from the current sync source, which will update the ping time.
const advancedTimestamp =
    assert.commandWorked(primaryColl.runCommand("insert", {documents: [{"advance": "timestamp"}]}))
        .operationTime;
jsTestLog(
    "Waiting for 'testNode' to receive heartbeats indicating that the primary has advanced its optime and that the sync source ping time has increased");
assert.soon(() => {
    const replSetGetStatus = assert.commandWorked(testNode.adminCommand({replSetGetStatus: 1}));

    const primaryTimestamp = replSetGetStatus.members[0].optime.ts;
    const receivedPrimaryHb = (bsonWoCompare(primaryTimestamp, advancedTimestamp) >= 0);

    const syncSourcePingTime = replSetGetStatus.members[2].pingMs;
    const receivedSyncSourceHb = (syncSourcePingTime > 0);

    return (receivedPrimaryHb && receivedSyncSourceHb);
});

hangOplogFetcherBeforeAdvancingLastFetched.off();

jsTestLog("Verifying that the node eventually syncs from the primary");
rst.awaitSyncSource(testNode, primary);

// Verify that the metric was incremented correctly.
serverStatus = assert.commandWorked(testNode.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(1, serverStatus.syncSource.numSyncSourceChangesDueToSignificantlyCloserNode);

rst.stopSet();
})();
