/**
 * Tests that nodes in the same region as the primary eventually do not sync across data centers. We
 * do this with a three-node replica set (P, S1, and S2). P and S1 are in the same data center,
 * while S2 is in its own data center. Initially, S1 syncs from S2, and S2 syncs from P. We verify
 * that eventually S1 will decide to sync from P, because it is in the same datacenter as P and thus
 * has a lower ping time.
 *
 * @tags: [
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {DataCenter, delayMessagesBetweenDataCenters, forceSyncSource} from "jstests/replsets/libs/sync_source.js";
import {setLogVerbosity} from "jstests/replsets/rslib.js";

const name = jsTestName();
const rst = new ReplSetTest({
    name,
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    nodeOptions: {
        setParameter: {
            // Set 'maxNumSyncSourceChangesPerHour' to a high value to remove the limit on how many
            // times nodes change sync sources in an hour.
            maxNumSyncSourceChangesPerHour: 99,
            writePeriodicNoops: true,
        },
    },
    settings: {
        // Set the heartbeat interval to a low value to reduce the amount of time spent waiting for
        // a heartbeat from sync source candidates.
        heartbeatIntervalMillis: 250,
    },
    useBridge: true,
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testNode = rst.getSecondaries()[0];
const secondary = rst.getSecondaries()[1];

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);
rst.awaitReplication();

const primaryDB = primary.getDB(name);
const primaryColl = primaryDB["testColl"];

assert.commandWorked(primaryColl.insert({"steady": "state"}, {writeConcern: {w: 3}}));

// Ensure we see the sync source progress logs.
setLogVerbosity(rst.nodes, {"replication": {"verbosity": 2}});

jsTestLog("Forcing sync sources for the secondaries");
const secondaryForceSyncSource = forceSyncSource(rst, secondary, primary);
const testNodeForceSyncSource = forceSyncSource(rst, testNode, secondary);

// Partition the nodes so that 'testNode' is in the same data center as the primary,
const westDC = new DataCenter("westDC", [primary, testNode]);
const eastDC = new DataCenter("eastDC", [secondary]);
// We set a high delay between data centers because we might only receive one or two heartbeats from
// our sync source. Our delay should create a sufficient ping time difference with just a single
// heartbeat.
delayMessagesBetweenDataCenters(westDC, eastDC, 300 /* delayMillis */);

// Hang 'testNode' in the oplog fetcher to ensure that sync source candidates are ahead of us.
const hangOplogFetcherBeforeAdvancingLastFetched = configureFailPoint(
    testNode,
    "hangOplogFetcherBeforeAdvancingLastFetched",
);

// Do a write to reduce the time spent waiting for a batch.
assert.commandWorked(primaryColl.insert({"make": "batch"}, {writeConcern: {w: 2}}));

hangOplogFetcherBeforeAdvancingLastFetched.wait();

secondaryForceSyncSource.off();
testNodeForceSyncSource.off();

const advancedTimestamp = assert.commandWorked(
    primaryColl.runCommand("insert", {documents: [{"advance": "timestamp"}]}),
).operationTime;
jsTestLog(
    `Waiting for 'testNode' to receive heartbeats. The primary should have advanced its optime to ${tojson(
        advancedTimestamp,
    )}`,
);
assert.soon(() => {
    const replSetGetStatus = assert.commandWorked(testNode.adminCommand({replSetGetStatus: 1}));

    // Wait for a heartbeat from the primary that shows that the primary's last timestamp is at
    // least 'advancedTimestamp'. This ensures the test node sees that the primary is ahead of
    // itself, and as a result, it can choose the primary as a sync source.
    const primaryTimestamp = replSetGetStatus.members[0].optime.ts;
    const receivedPrimaryHb = bsonWoCompare(primaryTimestamp, advancedTimestamp) >= 0;

    // Wait for enough heartbeats from the test node's current sync source so that the difference
    // between the sync source's and the primary's ping time is greater than
    // 'changeSyncSourceThresholdMillis'.
    const syncSourcePingTime = replSetGetStatus.members[2].pingMs;
    const primaryPingTime = replSetGetStatus.members[0].pingMs;
    const exceedsChangeSyncSourceThreshold = syncSourcePingTime - primaryPingTime > 5;

    return receivedPrimaryHb && exceedsChangeSyncSourceThreshold;
});

const replSetGetStatus = assert.commandWorked(testNode.adminCommand({replSetGetStatus: 1}));
jsTestLog(replSetGetStatus);

hangOplogFetcherBeforeAdvancingLastFetched.off();

jsTestLog("Verifying that the node eventually syncs from the primary");
rst.awaitSyncSource(testNode, primary);

rst.stopSet();
