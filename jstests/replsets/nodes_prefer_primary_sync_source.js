/**
 * Test that given primary and secondary eligible sync source candidates with similar ping times,
 * a syncing node will choose to sync from the primary node even if it has a higher ping time.
 * The primary and secondary candidate must be within the same data center for the syncing node
 * to make this choice. We do this with a four node replica set (P, S, farS, and test). We
 * create four data centers, and configure delays such that the test node is initially syncing
 * from the farthest node farS, and P and S have minimal ping difference where S is slightly
 * closer to the test node, but P and S still mimic being in the same data center.
 * Finally, we verify that the test node will decide to sync from P, since it is in a closer
 * data center, and we prefer it over S.
 *
 * @tags: [
 *  multiversion_incompatible,
 * ]
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
            // Set 'changeSyncSourceThresholdMillis' to a higher value to mitigate failures due to
            // network jitter. As we rely on ping times to select a sync source, a smaller threshold
            // may result in unexpected sync source selections due to small network delays.
            // We now consider nodes with ping difference <20ms to be in the same data center.
            changeSyncSourceThresholdMillis: 20,
        }
    },
    settings: {
        // Set the heartbeat interval to a low value to reduce the amount of time spent waiting for
        // a heartbeat from sync source candidates.
        heartbeatIntervalMillis: 100,
    },
    useBridge: true
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const [testNode, secondary, farSecondary] = rst.getSecondaries();

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
forceSyncSource(rst, farSecondary, primary);
// Force the test node to sync from a secondary in a far away data center.
const testNodeForceSyncSource = forceSyncSource(rst, testNode, farSecondary);

// Partition the nodes so that 'testNode' is far away from 'farSecondary', and the
// 'primary' and 'secondary' are slightly apart but mimic being in the same data center.
const westDC = new DataCenter("west", [farSecondary]);
const centralDC = new DataCenter("central", [primary]);
const eastCentralDC = new DataCenter("east central", [secondary]);
const eastDC = new DataCenter("east", [testNode]);

// Set the delay for adjacent data centers to 50 ms. Since the west data center and east data center
// are further apart, we delay messages between them by 300 ms.
// Mimic secondary and primary being in the same data center by making the delay between eastCentral
// data center and central data center very minimal.
delayMessagesBetweenDataCenters(westDC, centralDC, 50 /* delayMillis */);
delayMessagesBetweenDataCenters(centralDC, eastDC, 50 /* delayMillis */);
delayMessagesBetweenDataCenters(westDC, eastCentralDC, 52 /* delayMillis */);
delayMessagesBetweenDataCenters(eastCentralDC, eastDC, 48 /* delayMillis */);
delayMessagesBetweenDataCenters(centralDC, eastCentralDC, 2 /* delayMillis */);
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

let replSetGetStatus;
assert.soon(() => {
    replSetGetStatus = assert.commandWorked(testNode.adminCommand({replSetGetStatus: 1}));

    // Wait for a heartbeat from the target sync source that shows that the target sync source's
    // last timestamp is at least 'advancedTimestamp'. This ensures the test node sees that the
    // target sync source is ahead of itself, and as a result, it can decide to sync from the target
    // sync source.
    const primaryTimestamp = replSetGetStatus.members[0].optime.ts;
    const receivedCentralHb = (bsonWoCompare(primaryTimestamp, advancedTimestamp) >= 0);

    // Wait for enough heartbeats from the test node's current sync source so that our understanding
    // of the ping time is over 60 ms. This makes it likely to re-evaluate the sync source.
    const syncSourcePingTime = replSetGetStatus.members[3].pingMs;
    const receivedSyncSourceHb = (syncSourcePingTime > 60);

    // Wait for enough heartbeats from the desired sync source so that our understanding of the
    // ping time to that node is at least 'changeSyncSourceThresholdMillis' less than the ping time
    // to our current sync source.
    const primaryPingTime = replSetGetStatus.members[0].pingMs;
    const exceedsChangeSyncSourceThreshold = (syncSourcePingTime - primaryPingTime > 20);
    // Wait for primary ping to be larger than secondary ping, and their difference to be
    // considerably less than 'changeSyncSourceThresholdMillis' to ensure they are understood
    // as nodes in the same data center.
    const secondaryPingTime = replSetGetStatus.members[2].pingMs;
    const centralWithinChangeSyncSourceThreshold =
        (primaryPingTime - secondaryPingTime >= 0) && (primaryPingTime - secondaryPingTime < 20);

    return (receivedCentralHb && receivedSyncSourceHb && exceedsChangeSyncSourceThreshold &&
            centralWithinChangeSyncSourceThreshold);
}, tojson(replSetGetStatus));

jsTestLog(replSetGetStatus);

hangOplogFetcherBeforeAdvancingLastFetched.off();

jsTestLog("Verifying that the node eventually syncs from primary");
rst.awaitSyncSource(testNode, primary);

// Verify that the metric was incremented correctly.
serverStatus = assert.commandWorked(testNode.adminCommand({serverStatus: 1})).metrics.repl;
assert.eq(numSyncSourceChanges + 1,
          serverStatus.syncSource.numSyncSourceChangesDueToSignificantlyCloserNode);

rst.stopSet();
