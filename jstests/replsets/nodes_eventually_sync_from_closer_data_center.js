/**
 * Tests that nodes that are syncing from a node in a far away data center will eventually choose to
 * sync from a node in a closer data center. We do this with a three node replica set (P, S1, and
 * S2). We create a data center for each node and configure delays such that P's data center is
 * significantly farther away from S2 than S1's data center. Finally, we verify that S2 will decide
 * to sync from S1, since S1's data center is closer.
 *
 * @tags: [requires_fcv_46]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/libs/sync_source.js");
load('jstests/replsets/rslib.js');

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
const centralSecondary = rst.getSecondaries()[0];
const testNode = rst.getSecondaries()[1];

const primaryDB = primary.getDB(name);
const primaryColl = primaryDB["testColl"];

assert.commandWorked(primaryColl.insert({"steady": "state"}, {writeConcern: {w: 3}}));

// Ensure we see the sync source progress logs.
setLogVerbosity(rst.nodes, {"replication": {"verbosity": 2}});

let serverStatus = assert.commandWorked(testNode.adminCommand({serverStatus: 1})).metrics.repl;
const numSyncSourceChanges =
    serverStatus.syncSource.numSyncSourceChangesDueToSignificantlyCloserNode;

jsTestLog("Forcing sync sources for the secondaries");
forceSyncSource(rst, centralSecondary, primary);
// Force the test node to sync from the primary in the far away data center.
const testNodeForceSyncSource = forceSyncSource(rst, testNode, primary);

const westDC = new DataCenter("west", [primary]);
const centralDC = new DataCenter("centralDC", [centralSecondary]);
const eastDC = new DataCenter("east", [testNode]);

// Set the delay for adjacent data centers to 50 ms. Since the west data center and east data center
// are further apart, we delay messages between them by 300 ms.
delayMessagesBetweenDataCenters(westDC, centralDC, 50 /* delayMillis */);
delayMessagesBetweenDataCenters(centralDC, eastDC, 50 /* delayMillis */);
delayMessagesBetweenDataCenters(westDC, eastDC, 300 /* delayMillis */);

// Hang 'testNode' in the oplog fetcher to ensure that sync source candidates are ahead of us.
const hangOplogFetcherBeforeAdvancingLastFetched =
    configureFailPoint(testNode, "hangOplogFetcherBeforeAdvancingLastFetched");

// Do a write to reduce the time spent waiting for a batch.
assert.commandWorked(primaryColl.insert({"make": "batch"}, {writeConcern: {w: 2}}));

hangOplogFetcherBeforeAdvancingLastFetched.wait();
testNodeForceSyncSource.off();

const advancedTimestamp =
    assert.commandWorked(primaryColl.runCommand("insert", {documents: [{"advance": "timestamp"}]}))
        .operationTime;
jsTestLog(
    `Waiting for 'testNode' to receive heartbeats. The target sync source should have advanced its optime to ${
        tojson(advancedTimestamp)}`);
assert.soon(() => {
    const replSetGetStatus = assert.commandWorked(testNode.adminCommand({replSetGetStatus: 1}));

    // Wait for a heartbeat from the target sync source that shows that the target sync source's
    // last timestamp is at least 'advancedTimestamp'. This ensures the test node sees that the
    // target sync source is ahead of itself, and as a result, it can decide to sync from the target
    // sync source.
    const centralTimestamp = replSetGetStatus.members[1].optime.ts;
    const receivedCentralHb = (bsonWoCompare(centralTimestamp, advancedTimestamp) >= 0);

    // Wait for enough heartbeats from the test node's current sync source so that our understanding
    // of the ping time is over 60 ms. This makes it likely to re-evaluate the sync source.
    const syncSourcePingTime = replSetGetStatus.members[0].pingMs;
    const receivedSyncSourceHb = (syncSourcePingTime > 60);

    return (receivedCentralHb && receivedSyncSourceHb);
});

const replSetGetStatus = assert.commandWorked(testNode.adminCommand({replSetGetStatus: 1}));
jsTestLog(replSetGetStatus);

hangOplogFetcherBeforeAdvancingLastFetched.off();

jsTestLog("Verifying that the node eventually syncs from centralSecondary");
rst.awaitSyncSource(testNode, centralSecondary);

// Verify that the metric was incremented correctly.
serverStatus = assert.commandWorked(testNode.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(numSyncSourceChanges + 1,
          serverStatus.syncSource.numSyncSourceChangesDueToSignificantlyCloserNode);

rst.stopSet();
})();
