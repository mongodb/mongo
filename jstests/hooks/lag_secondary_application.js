/**
 * This hook runs induces a lag between the lastApplied and lastWritten on a random
 * secondary node in a replica set.
 */
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

const MIN_MS = 400;
const MAX_MS = 1000;

/* Pick a random millisecond value between 400 and 1000 for the lag value */
function randomMSFromInterval(minMS, maxMS) {  // min and max included
    return Math.floor(Math.random() * (maxMS - minMS + 1) + minMS)
}

/* Returns true if the error code indicates the node is currently shutting down. */
function isShutdownError(error) {
    // TODO (SERVER-54026): Remove check for error message once the shell correctly
    // propagates the error code.
    return error.code === ErrorCodes.ShutdownInProgress ||
        error.code === ErrorCodes.InterruptedAtShutdown ||
        error.message.includes("The server is in quiesce mode and will shut down");
}

function turnOffFailPointWithRetry(conn) {
    let retryRemaining = 5;
    while (retryRemaining > 0) {
        try {
            assert.commandWorked(conn.adminCommand({
                configureFailPoint: 'pauseBatchApplicationAfterWritingOplogEntries',
                mode: "off"
            }));
            jsTestLog("Resuming oplog application on secondary: " + conn);
            return;
        } catch (e) {
            if (isNetworkError(e)) {
                retryRemaining--;
                jsTestLog("Retrying turn off fail point on network error: " + tojson(e));
            } else {
                throw e;
            }
        }
    }
    jsTestLog("LagOplogApplication hook turn off failPoint with network retry failed. " +
              "The node is expected to be shutdown.");
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

    turnOffFailPointWithRetry(secondaryConn);
    return {ok: 1};
}

// Make sure this hook is resilient to network errors and shutdown errors that may come
// up in failover passthroughs.
let res;
try {
    // To make this hook work in kill primary passthroughs that can cause the initial connection
    // failing with network error, we need to use nodb:"" in the config then manually create the
    // connection so we can handle network errors.
    const conn = connect(TestData.connectionString);
    const topology = DiscoverTopology.findConnectedNodes(conn.getMongo());

    // Limit this hook to replica sets.
    if (topology.type !== Topology.kReplicaSet) {
        throw new Error('Unsupported topology configuration: ' + tojson(topology));
    }

    // Ensure there is at least one secondary.
    if (topology.nodes.length < 2) {
        throw new Error('Must have at least 2 nodes in the replica set: ' + tojson(topology));
    }

    const primary = topology.primary;
    const secondaries =
        (primary === undefined) ? topology.nodes : topology.nodes.filter(node => node !== primary);
    const randomSecondary = secondaries[Math.floor(Math.random() * secondaries.length)];
    const randomSecondaryConn = new Mongo(randomSecondary);
    res = lagLastApplied(randomSecondaryConn);
} catch (e) {
    // If the ReplicaSetMonitor cannot find a primary because it has stepped down or
    // been killed, it may take longer than 15 seconds for a new primary to step up.
    // Ignore this error until we find a new primary.
    const kReplicaSetMonitorErrors = [
        /^Could not find host matching read preference.*mode: "primary"/,
        /^can't connect to new replica set primary/
    ];

    if (isNetworkError(e)) {
        jsTestLog("Ignoring network error" + tojson(e));
    } else if (kReplicaSetMonitorErrors.some((regex) => {
                   return regex.test(e.message);
               })) {
        jsTestLog("Ignoring replica set monitor error" + tojson(e));
    } else if (isShutdownError(e)) {
        // It's possible that the secondary we passed in gets killed by the kill secondary hook.
        // During shutdown, mongod will respond to incoming hello requests with ShutdownInProgress
        // or InterruptedAtShutdown. This hook should ignore both cases and wait until we choose
        // a different secondary in a subsequent run.
        jsTestLog("Ignoring shutdown error" + tojson(e));
    } else {
        jsTestLog(`lag_secondary_application unexpected error: ${tojson(e)}`);
        throw e;
    }

    res = {ok: 1};
}

assert.commandWorked(res, "lag_secondary_application hook failed: " + tojson(res));
