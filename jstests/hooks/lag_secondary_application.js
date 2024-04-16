/**
 * This hook runs induces a lag between the lastApplied and lastWritten on a random
 * secondary node in a replica set.
 */
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const MIN_MS = 400;
const MAX_MS = 1000;

/* Pick a random millisecond value between 400 and 1000 for the lag value */
function randomMSFromInterval(minMS, maxMS) {  // min and max included
    return Math.floor(Math.random() * (maxMS - minMS + 1) + minMS)
}

/**
 * Enables the 'pauseBatchApplicationAfterWritingOplogEntries' failpoint on a secondary
 * node. This failpoint will pause oplog application after writing entries to the oplog
 * but before applying those changes to data collections. Therefore, we will induce lag
 * between the lastWritten and lastApplied timestamps.
 */
function lagLastApplied(secondaryConn) {
    const randMS = randomMSFromInterval(MIN_MS, MAX_MS);
    jsTestLog("Pausing oplog application for " + randMS + " ms on secondary: " + secondaryConn);

    assert.commandWorked(secondaryConn.adminCommand(
        {configureFailPoint: 'pauseBatchApplicationAfterWritingOplogEntries', mode: "alwaysOn"}));
    // Induce a random millisecond lag and turn off the failpoint.
    sleep(randMS);
    assert.commandWorked(secondaryConn.adminCommand(
        {configureFailPoint: 'pauseBatchApplicationAfterWritingOplogEntries', mode: "off"}));

    jsTestLog("Resuming oplog application on secondary: " + secondaryConn);
}

const topology = DiscoverTopology.findConnectedNodes(db.getMongo());

// Limit this hook to replica sets.
if (topology.type !== Topology.kReplicaSet) {
    throw new Error('Unsupported topology configuration: ' + tojson(topology));
}

// Ensure there is at least one secondary.
if (topology.nodes.length < 2) {
    throw new Error('Must have at least 2 nodes in the replica set: ' + tojson(topology));
}

const secondaries = FixtureHelpers.getSecondaries(db);
const randomSecondary = secondaries[Math.floor(Math.random() * secondaries.length)];
lagLastApplied(randomSecondary);
