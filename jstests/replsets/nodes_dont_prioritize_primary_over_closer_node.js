/**
 * Test that nodes will not prioritize choosing the primary as a sync source over a candidate
 * node that is in a closer data center than the primary. We do this with a four node replica
 * set (P, S, targetS, and test). We create three data centers, and configure delays such that
 * the test node is initially syncing from S, which is in the same datacenter as P, and the
 * targetS is in the data center closest to the test node.
 * Finally, we verify that the test node will decide to sync from targetS, since it is in the
 * closest data center.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    DataCenter,
    delayMessagesBetweenDataCenters,
    forceSyncSource
} from "jstests/replsets/libs/sync_source.js";
import {setLogVerbosity} from "jstests/replsets/rslib.js";

const name = jsTestName();
const rst = new ReplSetTest({
    name,
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
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
rst.initiate();

const primary = rst.getPrimary();
const [testNode, targetSecondary, secondary] = rst.getSecondaries();

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
rst.awaitReplication();

const primaryDB = primary.getDB(name);
const primaryColl = primaryDB["testColl"];

assert.commandWorked(primaryColl.insert({"steady": "state"}, {writeConcern: {w: 4}}));

// Ensure we see the sync source progress logs.
setLogVerbosity(rst.nodes, {"replication": {"verbosity": 2}});

let serverStatus = assert.commandWorked(testNode.adminCommand({serverStatus: 1})).metrics.repl;
const numSyncSourceChanges =
    serverStatus.syncSource.numSyncSourceChangesDueToSignificantlyCloserNode;

jsTestLog("Forcing sync sources for the secondaries");
forceSyncSource(rst, secondary, primary);
forceSyncSource(rst, targetSecondary, primary);
// Force the test node to sync from a secondary in a far away data center.
const testNodeForceSyncSource = forceSyncSource(rst, testNode, secondary);

// Partition the nodes so that 'testNode' is far away from 'secondary' and 'primary',
// and 'targetSecondary' is in a closesr data center.
const westDC = new DataCenter("west", [primary, secondary]);
const centralDC = new DataCenter("central", [targetSecondary]);
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
assert.commandWorked(primaryColl.insert({"make": "batch"}, {writeConcern: {w: 3}}));

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
    const targetTimestamp = replSetGetStatus.members[2].optime.ts;
    const receivedCentralHb = (bsonWoCompare(targetTimestamp, advancedTimestamp) >= 0);

    // Wait for enough heartbeats from the test node's current sync source so that our understanding
    // of the ping time is over 60 ms. This makes it likely to re-evaluate the sync source.
    const syncSourcePingTime = replSetGetStatus.members[3].pingMs;
    const receivedSyncSourceHb = (syncSourcePingTime > 60);

    // Wait for enough heartbeats from the desired sync source so that our understanding of the
    // ping time to that node is at least 'changeSyncSourceThresholdMillis' less than the ping time
    // to our current sync source.
    const targetPingTime = replSetGetStatus.members[2].pingMs;
    const exceedsChangeSyncSourceThreshold = (syncSourcePingTime - targetPingTime > 5);
    // Wait for primary and secondary ping difference to be within
    // 'changeSyncSourceThresholdMillis' as they are in the same data center.
    const primaryPingTime = replSetGetStatus.members[0].pingMs;
    const westWithinChangeSyncSourceThreshold = Math.abs(primaryPingTime - syncSourcePingTime) < 5;

    return (receivedCentralHb && receivedSyncSourceHb && exceedsChangeSyncSourceThreshold &&
            westWithinChangeSyncSourceThreshold);
});

const replSetGetStatus = assert.commandWorked(testNode.adminCommand({replSetGetStatus: 1}));
jsTestLog(replSetGetStatus);

hangOplogFetcherBeforeAdvancingLastFetched.off();

jsTestLog("Verifying that the node eventually syncs from target");
rst.awaitSyncSource(testNode, targetSecondary);

// Verify that the metric was incremented correctly.
serverStatus = assert.commandWorked(testNode.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(numSyncSourceChanges + 1,
          serverStatus.syncSource.numSyncSourceChangesDueToSignificantlyCloserNode);

rst.stopSet();
