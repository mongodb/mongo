/**
 * Tests that a config shard node performing an aggregation on 'config.*' to refresh the catalog
 * cache, where this aggregation ends up targeting the requesting node itself, does not end up in a
 * deadlock.
 *
 * @tags: [
 *  requires_sharding,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function forceReadPreferenceNearestToTargetPrimary(replSet) {
    // Internal aggregations on the config server use 'readPreference: nearest'. For the issue to
    // reproduce, server selection must end up targeting the primary.
    //
    // SDAM server selection will discard anything with a round trip time (RTT) above
    // defaultLocalThresholdMillis(15). RTT is computed from the hello command on init, but updated
    // with ping responses. The shard keeps a single Replica Set Monitor, so delaying the 'hello'
    // command is unhelpful. Instead, we override the RTT from ping, and wait until at least a ping
    // for 3 nodes has been done (even if we only override 2 of them).
    //
    // Turn on serverPingMonitorSetRTT on primary to simulate slow 'ping' from secondaries.
    const kDelayMicros = 200 * 1000;
    var hosts = {};
    replSet.getSecondaries().forEach((sec) => {
        // Current RTT accounts for kRttAlpha(0.2) of new RTT.
        // Use a value large enough so the computation is guaranteed to exceed
        // defaultLocalThresholdMillis in a single iteration.
        hosts[sec.host] = kDelayMicros;
    });
    const monitorDelayFailPoint =
        configureFailPoint(replSet.getPrimary(), "serverPingMonitorSetRTT", hosts);
    // Wait for a refresh on each node.
    monitorDelayFailPoint.wait({timesEntered: 3});

    return () => {
        monitorDelayFailPoint.off();
    };
}

const logLevel = tojson({sharding: {shardingCatalogRefresh: 1}, command: 2, query: 1});
var st = new ShardingTest({
    shards: 1,
    rs: {
        nodes: [
            {/* primary */},
            {/* secondary */ rsConfig: {priority: 0}},
            {/* secondary */ rsConfig: {priority: 0}},
        ],
        setParameter: {logComponentVerbosity: logLevel, heartBeatFrequencyMs: 1000},
    },
    configShard: true,
    other: {enableBalancer: false},
});

const dbName = "test";
function nsFor(coll) {
    return dbName + "." + coll;
}

// The number of parallel queries should be at least as large as the amount of threads in the thread
// pool of the CatalogCache.
const nParallelQueries = 6;
let collections = Array.from({length: nParallelQueries}, (_, i) => "coll_" + i);

// Shard collections and insert a document.
collections.forEach((coll) => {
    assert.commandWorked(st.s.adminCommand({shardCollection: nsFor(coll), key: {_id: 1}}));
    st.s.getDB(dbName)[coll].insertOne({x: 1});
});

// createCollectionCoordinator issues a fire and forget routing table cache update, which can
// complete after 'flushRouterConfig', messing our test setup. Perform a find operation to ensure
// the refresh is finished.
collections.forEach((coll) => {
    assert.commandWorked(st.s.getDB(dbName).runCommand({find: coll}));
});

const configShard = st.shard0;
// Clear catalog cache on shard.
assert.commandWorked(configShard.adminCommand({flushRouterConfig: 1}));

const cleanUpFn = forceReadPreferenceNearestToTargetPrimary(st.rs0);

// Block CollectionCache lookups on dbName.
const cacheFP = configureFailPoint(configShard, "blockCollectionCacheLookup", {nss: dbName});

// Issue parallel queries to all test collections.
let parallelShellArr = collections.map((coll) => {
    return startParallelShell(
        funWithArgs(function(dbName, collName) {
            // We want the query to happen on the primary.
            db.getMongo().setReadPref("primary");
            const testDB = db.getSiblingDB(dbName);
            // Use a $unionWith to cause the shard to perform some routing internally. Otherwise
            // there is no need for a refresh on the shard.
            assert.commandWorked(testDB.runCommand(
                {aggregate: collName, pipeline: [{$unionWith: {coll: collName}}], cursor: {}}));
            jsTestLog("Finished query for " + collName);
        }, dbName, coll), st.s.port);
});

// Wait for all queries to refresh. Unrelated refreshes can happen, so timesEntered is not reliable.
// Wait for the log message instead.
assert.soon(() =>
                checkLog.checkContainsWithCountJson(configShard, 9131800, {}, collections.length));

// Unblock threads.
cacheFP.off();

jsTestLog("Join parallel queries");
parallelShellArr.forEach((parallelShell) => parallelShell());

cleanUpFn();

st.stop();
