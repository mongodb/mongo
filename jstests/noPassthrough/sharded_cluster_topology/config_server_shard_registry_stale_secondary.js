/**
 * Tests a config server's ShardRegistry refresh does not get indefinitely stuck against a stale
 * config replica.
 *
 * @tags: [
 *   requires_fcv_83,
 *   config_shard_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

// Skip consistency checks that read from the config server, since this test intentionally degrades
// the config RS by stopping replication and killing members.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckRoutingTableConsistency = true;
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckMetadataConsistency = true;

const dbName = jsTestName();

const st = new ShardingTest({
    shards: 1,
    config: 3,
    useBridge: true,
    configReplSetTestOptions: {settings: {chainingAllowed: false}},
});

const configRS = st.configRS;
const configPrimary = configRS.getPrimary();
const configSecondaries = configRS.getSecondaries();
const staleSecondary = configSecondaries[0];
const healthySecondary = configSecondaries[1];

// Pause the periodic ShardRegistry pinger so its background refresh cannot race with the
// manually triggered _flushShardRegistry call below. This makes the test deterministic: the
// only reload that runs during the partition window is the one we explicitly invoke.
jsTestLog("Pausing periodic ShardRegistry pinger on primary via failpoint...");
const periodicPingFP = configureFailPoint(configPrimary, "hangShardRegistryPeriodicPing");
periodicPingFP.wait();

// ---------------------------------------------------------------------------
// Step 1: Establish initial configTime. All config RS members are in sync.
// ---------------------------------------------------------------------------
jsTestLog("Step 1: Establishing initial configTime with writes");
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: dbName + ".coll", key: {_id: 1}}));
assert.commandWorked(st.s.getDB(dbName).coll.insert({_id: "setup"}));
configRS.awaitReplication();

// ---------------------------------------------------------------------------
// Step 2: Stop replication on one secondary to make it stale.
// ---------------------------------------------------------------------------
jsTestLog(
    "Step 2: Stopping replication on config secondary " + staleSecondary.host + " to make it stale",
);
stopServerReplication(staleSecondary);

// ---------------------------------------------------------------------------
// Step 3: Isolate the config primary's RSM so the only reachable member is the stale secondary.
// _flushShardRegistry on the primary then drives an internal find on config.shards via the
// loopback ShardRegistry refresh path. That find attaches the configsvr's current configTime
// as afterClusterTime; the stale secondary's apply is paused (Step 2) so it cannot satisfy it.
// With the fix the find times out via findShardsOnConfigTimeoutMS; without the fix it hangs.
//
// The shard registry's internal find uses $readPreference: nearest, so reachability — not
// PRIMARY state — drives routing. Bridge isolation is enough; no stepdown needed.
// ---------------------------------------------------------------------------
jsTestLog("Step 3: Verifying ShardRegistry refresh times out against stale secondary");

// `disconnect()` closes existing bridge proxy connections in both directions and rejects new ones,
// which forces subsequent RSM heartbeats and pool establishment to fail. We use it for both the
// healthy secondary and self so the only routable RSM member from configPrimary's perspective is
// the stale secondary.
jsTestLog("Disconnecting primary from healthy secondary via bridge...");
configPrimary.disconnect(healthySecondary);
jsTestLog("Disconnecting primary from itself via bridge...");
configPrimary.disconnect(configPrimary);

jsTestLog("Waiting for configPrimary RSM to mark self + healthy secondary as Unknown...");
assert.soon(
    () => {
        const stats = assert.commandWorked(configPrimary.adminCommand({connPoolStats: 1}));
        const rs = stats.replicaSets && stats.replicaSets[configRS.name];
        if (!rs || !rs.hosts) {
            return false;
        }
        const reachable = rs.hosts.filter((h) => h.ismaster || h.secondary);
        return reachable.length === 1 && reachable[0].addr === staleSecondary.host;
    },
    "Timed out waiting for configPrimary RSM to converge to 'only stale reachable'",
    60 * 1000,
);

jsTestLog(
    "Config primary isolated from healthy secondary and self. " +
        "Triggering _flushShardRegistry to force a shard registry refresh...",
);

// _flushShardRegistry triggers ShardRegistry::reloadForRecovery() which calls the same
// _exhaustiveFindOnConfig code path. The RSM routes to the stale secondary, the afterClusterTime
// cannot be satisfied, and the find times out on its own. The error propagates back, proving the
// config server is no longer stuck indefinitely.
//
// Without a proper maxTimeMS, this command would hang forever.
const flushResult = configPrimary.adminCommand({_flushShardRegistry: 1});

assert.commandFailedWithCode(
    flushResult,
    ErrorCodes.MaxTimeMSExpired,
    "Expected _flushShardRegistry to fail because the shard registry refresh was " +
        "directed to the stale secondary which cannot satisfy afterClusterTime, " +
        "and the find's own maxTimeMS (from findShardsOnConfigTimeoutMS) expired. " +
        "Got: " +
        tojson(flushResult),
);

jsTestLog(
    "Part B complete: _flushShardRegistry failed with MaxTimeMSExpired. " +
        "The find's own maxTimeMS (from findShardsOnConfigTimeoutMS) caused a timeout.",
);

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------
jsTestLog("Cleaning up...");

configPrimary.acceptConnectionsFrom(configPrimary);
configPrimary.reconnect(healthySecondary);
periodicPingFP.off();

restartServerReplication(staleSecondary);
configRS.awaitSecondaryNodes();

st.stop();
