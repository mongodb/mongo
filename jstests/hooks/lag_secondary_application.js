/**
 * This hook runs induces a lag between the lastApplied and lastWritten on a random
 * secondary node in a replica set.
 */
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import newMongoWithRetry from "jstests/libs/retryable_mongo.js";

const MIN_MS = 400;
const MAX_MS = 1000;

/* Pick a random millisecond value between 400 and 1000 for the lag value */
function randomMSFromInterval(minMS, maxMS) {
    // min and max included
    return Math.floor(Math.random() * (maxMS - minMS + 1) + minMS);
}

/* Returns true if the error code indicates the node is currently shutting down. */
function isShutdownError(error) {
    // TODO (SERVER-54026): Remove check for error messages once the shell correctly
    // propagates the error code.
    return (
        error.code === ErrorCodes.ShutdownInProgress ||
        error.code === ErrorCodes.InterruptedAtShutdown ||
        error.message.includes("The server is in quiesce mode and will shut down") ||
        error.message.includes("interrupted at shutdown")
    );
}

function turnOffFailPointWithRetry(conn) {
    let retryRemaining = 5;
    while (retryRemaining > 0) {
        try {
            assert.commandWorked(
                conn.adminCommand({
                    configureFailPoint: "pauseBatchApplicationAfterWritingOplogEntries",
                    mode: "off",
                }),
            );
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
    jsTestLog(
        "LagOplogApplication hook turn off failPoint with network retry failed. " +
            "The node is expected to be shutdown.",
    );
}
/**
 * Enables the 'pauseBatchApplicationAfterWritingOplogEntries' failpoint on each secondary
 * in 'secondaryConns'. This failpoint will pause oplog application after writing entries to
 * the oplog but before applying those changes to data collections. Therefore, we will induce
 * lag between the lastWritten and lastApplied timestamps on all of those secondaries at once.
 */
function lagLastApplied(secondaryConns) {
    const randMS = randomMSFromInterval(MIN_MS, MAX_MS);
    const enabledConns = [];
    let lagError;
    let cleanupError;
    try {
        for (const secondaryConn of secondaryConns) {
            jsTestLog(
                "Pausing oplog application for " + randMS + " ms on secondary: " + secondaryConn,
            );
            assert.commandWorked(
                secondaryConn.adminCommand({
                    configureFailPoint: "pauseBatchApplicationAfterWritingOplogEntries",
                    mode: "alwaysOn",
                }),
            );
            enabledConns.push(secondaryConn);
        }
        // Induce a random millisecond lag.
        sleep(randMS);
    } catch (e) {
        lagError = e;
    } finally {
        // Turn off every enabled failpoint before propagating lagError. ContinuousAddRemoveShard can
        // shut a selected shard down during this loop; continue after that failure so the
        // remaining secondaries are not left paused.
        for (const secondaryConn of enabledConns) {
            try {
                turnOffFailPointWithRetry(secondaryConn);
            } catch (e) {
                // Prefer an unexpected error over an earlier shutdown error: the outer catch
                // ignores shutdown, which would otherwise hide a real cleanup failure.
                if (!cleanupError || (isShutdownError(cleanupError) && !isShutdownError(e))) {
                    cleanupError = e;
                } else {
                    jsTestLog(
                        "Error turning off failpoint on secondary " +
                            secondaryConn +
                            " during cleanup: " +
                            tojson(e),
                    );
                }
            }
        }
    }
    if (lagError) {
        if (cleanupError) {
            jsTestLog("Failpoint cleanup also failed: " + tojson(cleanupError));
        }
        throw lagError;
    }
    if (cleanupError) {
        throw cleanupError;
    }
    return {ok: 1};
}

function replicaSetHasSecondary(rs) {
    return rs.type === Topology.kReplicaSet && rs.nodes.length >= 2;
}

/**
 * Returns a replica set topology that has at least one secondary. For a replica set, that is
 * the cluster itself. For a sharded cluster, a random non-empty subset of shards with more than one node is
 * chosen. Single-node shards are skipped because this hook needs a secondary to lag.
 */
function pickReplicaSetWithSecondary(topology) {
    if (topology.type === Topology.kReplicaSet) {
        if (!replicaSetHasSecondary(topology)) {
            throw new Error("Must have at least 2 nodes in the replica set: " + tojson(topology));
        }
        return [topology];
    }

    const shards = Object.values(topology.shards);
    if (shards.length < 1) {
        throw new Error("Must have at least 1 shard in the sharded cluster: " + tojson(topology));
    }

    const shardsWithSecondary = shards.filter(replicaSetHasSecondary);
    const csrs = topology.configsvr;
    if (shardsWithSecondary.length === 0 && !replicaSetHasSecondary(csrs)) {
        throw new Error(
            "Must have at least 1 shard with 2 or more nodes in the sharded cluster: " +
                tojson(topology),
        );
    }

    if (replicaSetHasSecondary(csrs)) {
        shardsWithSecondary.push(csrs);
    }

    // Pick random non-empty subset of shards
    for (let i = shardsWithSecondary.length - 1; i > 0; i--) {
        const j = Math.floor(Math.random() * (i + 1));
        const swap = shardsWithSecondary[i];
        shardsWithSecondary[i] = shardsWithSecondary[j];
        shardsWithSecondary[j] = swap;
    }
    const count = Math.floor(Math.random() * shardsWithSecondary.length) + 1;
    return shardsWithSecondary.slice(0, count);
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

    // Limit this hook to replica sets and sharded clusters.
    if (topology.type !== Topology.kReplicaSet && topology.type !== Topology.kShardedCluster) {
        throw new Error("Unsupported topology configuration: " + tojson(topology));
    }

    const replicaSets = pickReplicaSetWithSecondary(topology);
    const secondaryConns = [];
    for (const rs of replicaSets) {
        const primary = rs.primary;
        const secondaries =
            primary === undefined ? rs.nodes : rs.nodes.filter((node) => node !== primary);
        const randomSecondary = secondaries[Math.floor(Math.random() * secondaries.length)];
        secondaryConns.push(newMongoWithRetry(randomSecondary));
    }
    res = lagLastApplied(secondaryConns);
} catch (e) {
    // If the ReplicaSetMonitor cannot find a primary because it has stepped down or
    // been killed, it may take longer than 15 seconds for a new primary to step up.
    // Ignore this error until we find a new primary.
    const kReplicaSetMonitorErrors = [
        /^Could not find host matching read preference.*mode: "primary"/,
        /^can't connect to new replica set primary/,
    ];

    if (isNetworkError(e)) {
        jsTestLog("Ignoring network error" + tojson(e));
    } else if (
        e.code === ErrorCodes.FailedToSatisfyReadPreference ||
        kReplicaSetMonitorErrors.some((regex) => {
            return regex.test(e.message);
        })
    ) {
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
