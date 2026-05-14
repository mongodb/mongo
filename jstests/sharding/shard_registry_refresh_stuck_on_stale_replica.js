/**
 * Regression test for the config server's ShardRegistry refresh blocking indefinitely against a
 * stale config replica.
 *
 * Scenario reproduced:
 *  - The config server replica set has three nodes.
 *  - One secondary is artificially held back so its applied oplog does not catch up to the
 *    primary's. This makes it unable to satisfy afterClusterTime recency requirements for any new
 *    cluster time written by the primary.
 *  - A shard-topology change (addShard) is performed on the primary. This bumps the cluster time
 *    and, in the buggy build, causes a subsequent ShardRegistry refresh on mongos to read with
 *    afterClusterTime > stale node's lastApplied.
 *  - The refresh on mongos must still complete within a bounded time. With the fix, the refresh
 *    re-routes away from the stale secondary; without the fix, the request stalls indefinitely.
 *
 * @tags: [
 *   requires_sharding,
 *   config_shard_incompatible,
 *   does_not_support_stepdowns,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// The bug only reproduces when the config server is its own replica set, distinct from any data
// shard. The 'config_shard_incompatible' tag above keeps the suite from co-locating the two.
const st = new ShardingTest({
    name: "shard_registry_refresh_stuck_on_stale_replica",
    shards: 1,
    rs: {nodes: 1},
    config: 3,
    other: {
        // Lower heartbeat frequency so the replica-set monitor on mongos notices state changes
        // promptly. Without this the test could falsely pass simply because the monitor would not
        // even consider the stale secondary inside the bounded wait.
        configOptions: {setParameter: {heartbeatIntervalMillis: 500}},
    },
});

const configRS = st.configRS;
const configPrimary = configRS.getPrimary();
const configSecondaries = configRS.getSecondaries();
assert.eq(2, configSecondaries.length, "expected exactly two config secondaries");

// Pick one secondary to render stale. We freeze its oplog application via the stopReplProducer
// failpoint, which is the same mechanism used by jstests/replsets/oplog_fetch_lag_metric.js to
// induce replication lag deterministically.
const staleSecondary = configSecondaries[0];
const liveSecondary = configSecondaries[1];

jsTestLog("Freezing oplog production on stale config secondary: " + staleSecondary.host);
const stopProducerFp = configureFailPoint(staleSecondary, "stopReplProducer");
stopProducerFp.wait({maxTimeMS: 60 * 1000});

// Confirm the live secondary is still healthy before driving topology changes through. This
// guarantees the primary can still commit writes with w:majority (primary + liveSecondary form a
// majority), which is the realistic shape of the bug: the stale node is a minority drag, not a
// quorum failure.
configRS.awaitSecondaryNodes(60 * 1000, [liveSecondary]);

// Force mongos to drop any cached shard registry state so the next routing operation has to issue
// a fresh refresh against the config server.
assert.commandWorked(st.s.adminCommand({flushRouterConfig: 1}));

// Start a second replica set to be added as a new shard. The addShard write goes through the
// config primary and bumps cluster time; this is the operation that, in the buggy build, causes a
// subsequent ShardRegistry refresh to deadlock against the stale secondary's afterClusterTime
// check.
jsTestLog("Starting auxiliary replica set to addShard");
const auxRS = new ReplSetTest({name: "shardRegistryRefreshAux", nodes: 1});
auxRS.startSet({shardsvr: ""});
auxRS.initiate();
auxRS.getPrimary();

const newShardName = "shardRegistryRefreshAuxShard";
assert.commandWorked(st.s.adminCommand({addShard: auxRS.getURL(), name: newShardName}));

// Sanity check: the primary's view of config.shards now contains the new shard.
assert.neq(
    null,
    configPrimary.getDB("config").shards.findOne({_id: newShardName}),
    "addShard did not commit on the config primary",
);

// The core assertion: a routing operation that requires the ShardRegistry to be refreshed must
// return within a bounded time, even though one config secondary is unrecoverably stale.
//
// listShards is a thin wrapper around the ShardRegistry; if the registry refresh is stuck, this
// command will block indefinitely. We wrap it in assert.soon with a 30s ceiling so a regression
// shows up as a test timeout rather than a hanging suite.
const refreshDeadlineMs = 30 * 1000;
jsTestLog("Asserting ShardRegistry refresh on mongos completes within " + refreshDeadlineMs + "ms");

let listShardsResult;
const refreshStart = Date.now();
assert.soon(
    function () {
        // flushRouterConfig forces the next routing operation through the ShardRegistry refresh
        // path each iteration, so we are exercising the refresh code and not a cached response.
        assert.commandWorked(st.s.adminCommand({flushRouterConfig: 1}));
        try {
            listShardsResult = st.s.adminCommand({listShards: 1});
            return listShardsResult && listShardsResult.ok === 1;
        } catch (e) {
            jsTestLog("listShards iteration threw: " + tojson(e));
            return false;
        }
    },
    "ShardRegistry refresh on mongos did not complete within " + refreshDeadlineMs + "ms while one " +
        "config secondary was held stale; the refresh appears stuck against the stale replica.",
    refreshDeadlineMs,
    1000,
);
const refreshElapsedMs = Date.now() - refreshStart;
jsTestLog("ShardRegistry refresh completed in " + refreshElapsedMs + "ms");

// The refresh must have produced a complete view that includes the newly-added shard, otherwise
// the test would be unable to distinguish a real refresh from a cached stub.
assert.eq(1, listShardsResult.ok, "listShards did not return ok");
const observedShardIds = listShardsResult.shards.map((s) => s._id);
assert.contains(
    newShardName,
    observedShardIds,
    "ShardRegistry refresh returned but did not include the freshly-added shard; result was: " +
        tojson(listShardsResult),
);

jsTestLog("Releasing stopReplProducer failpoint on stale secondary");
stopProducerFp.off();

// Wait for the previously-stale secondary to catch up so ShardingTest's teardown can perform its
// usual majority-write health checks without timing out.
configRS.awaitReplication(60 * 1000);

// removeShard isn't strictly required for the regression check, but it keeps the cluster tidy
// before stopping the auxiliary replica set.
assert.soon(
    function () {
        const res = st.s.adminCommand({removeShard: newShardName});
        if (!res.ok) {
            return false;
        }
        return res.state === "completed";
    },
    "removeShard did not reach 'completed' state for " + newShardName,
    5 * 60 * 1000,
    1000,
);

auxRS.stopSet();
st.stop();
