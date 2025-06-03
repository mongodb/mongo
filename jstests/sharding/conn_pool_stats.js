/**
 * Tests for the connPoolStats command.
 *
 * Incompatible because it makes assertions about the specific number of connections used, which
 * don't account for background activity on a config server.
 * @tags: [
 *   config_shard_incompatible,
 *   requires_fcv_63,
 * ]
 */
import {assertHasConnPoolStats, launchFinds} from "jstests/libs/conn_pool_helpers.js";
import {configureFailPoint, configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 1});

// Run the connPoolStats command
let stats = st.s.getDB("admin").runCommand({connPoolStats: 1});

// Validate output
printjson(stats);
assert.commandWorked(stats);
assert("replicaSets" in stats);
assert("hosts" in stats);
assert("numClientConnections" in stats);
assert("numAScopedConnections" in stats);
assert("totalInUse" in stats);
assert("totalAvailable" in stats);
assert("totalCreated" in stats);
assert.lte(stats["totalInUse"] + stats["totalAvailable"], stats["totalCreated"], tojson(stats));

assert("pools" in stats);
assert("totalRefreshing" in stats);
assert.lte(stats["totalInUse"] + stats["totalAvailable"] + stats["totalRefreshing"],
           stats["totalCreated"],
           tojson(stats));

assert("totalRefreshed" in stats);

assert("acquisitionWaitTimes" in stats);

// Check that connection wait time histograms are consistent.
function assertHistogramIsSumOfChildren(parentStats, childrenStats) {
    const parentHist = parentStats["acquisitionWaitTimes"];
    var childHists = [];
    for (const childKey in childrenStats) {
        const child = childrenStats[childKey];
        if ((typeof (child) === 'object') && ("acquisitionWaitTimes" in child)) {
            childHists.push(child["acquisitionWaitTimes"]);
        }
    }

    for (const bucket in parentHist) {
        var total = 0;
        let bucketValue = (bucket == "totalCount") ? (h => h[bucket]) : (h => h[bucket].count);
        for (const childHist of childHists) {
            total += bucketValue(childHist);
        }
        assert.eq(bucketValue(parentHist), total, bucket);
    }
}
assertHistogramIsSumOfChildren(stats, stats["hosts"]);
assertHistogramIsSumOfChildren(stats, stats["pools"]);
for (const poolName in stats["pools"]) {
    assertHistogramIsSumOfChildren(stats["pools"][poolName], stats["pools"][poolName]);
}

function neverUsedMetricTest(kDbName = 'test') {
    const mongos = st.s0;
    const primary = st.rs0.getPrimary();
    const mongosDB = mongos.getDB(kDbName);

    const cfg = primary.getDB('local').system.replset.findOne();
    const allHosts = cfg.members.map(x => x.host);
    const primaryOnly = [primary.name];

    let currentCheckNum = 0;
    let toRefreshTimeoutMS = 5000;
    let threads = [];

    [1, 2, 3].forEach(v => assert.commandWorked(mongosDB.test.insert({x: v})));
    st.rs0.awaitReplication();

    const numPools = assert.commandWorked(mongos.adminCommand(
        {"getParameter": 1, "taskExecutorPoolSize": 1}))["taskExecutorPoolSize"];

    // Bump up number of pooled connections to 15
    const poolMinSize = 15;
    assert.commandWorked(mongos.adminCommand({
        "setParameter": 1,
        ShardingTaskExecutorPoolMinSize: poolMinSize,
        ShardingTaskExecutorPoolRefreshRequirementMS: toRefreshTimeoutMS
    }));
    const totalConns = numPools * poolMinSize;

    // Launch 5 blocked finds and verify that 5 connections are in-use while the remaining ones
    // are available
    const fpRs = configureFailPointForRS(st.rs0.nodes,
                                         "waitInFindBeforeMakingBatch",
                                         {shouldCheckForInterrupt: true, nss: kDbName + ".test"});
    const numFinds = 5;
    launchFinds(mongos, threads, {times: numFinds, readPref: "primary"}, currentCheckNum);
    currentCheckNum =
        assertHasConnPoolStats(mongos,
                               allHosts,
                               {active: numFinds, ready: totalConns - numFinds, hosts: primaryOnly},
                               currentCheckNum);

    // Reduce pool size to drop some available connections, verify that these were never used to
    // run an operation during their lifetime
    const reducedSize = 7;
    assert.commandWorked(mongos.adminCommand({
        "setParameter": 1,
        ShardingTaskExecutorPoolMinSize: reducedSize,
        ShardingTaskExecutorPoolMaxSize: reducedSize,
    }));
    const reducedTotalConns = numPools * reducedSize;
    const neverUsedConns = totalConns - reducedTotalConns;
    currentCheckNum = assertHasConnPoolStats(
        mongos,
        allHosts,
        {
            hosts: primaryOnly,
            checkStatsFunc: function(stats) {
                return stats.available == reducedTotalConns - numFinds && stats.inUse == numFinds &&
                    stats.wasNeverUsed == neverUsedConns;
            }
        },
        currentCheckNum);
    fpRs.off();
    threads.forEach(t => t.join());
}

neverUsedMetricTest();

// Enable the following fail point to refresh connections after every command.
let refreshConnectionFailPoint =
    configureFailPoint(st.s.getDB("admin"), "refreshConnectionAfterEveryCommand");

let latestTotalRefreshed = stats["totalRefreshed"];

// Iterate over totalRefreshed stat to assert it increases over time.
const totalIterations = 5;
for (let counter = 1; counter <= totalIterations; ++counter) {
    jsTest.log("Testing totalRefreshed, iteration " + counter);
    // Issue a find command to force a connection refresh
    st.s.getDB("admin").runCommand({"find": "hello world"});
    assert.soon(function() {
        stats = st.s.getDB("admin").runCommand({connPoolStats: 1});
        assert.commandWorked(stats);
        const result = latestTotalRefreshed < stats["totalRefreshed"];
        jsTest.log(tojson({
            "latest total refreshed": latestTotalRefreshed,
            "current total refreshed": stats["totalRefreshed"]
        }));
        latestTotalRefreshed = stats["totalRefreshed"];
        return result;
    }, "totalRefreshed stat did not increase within the expected time", 5000);
}

refreshConnectionFailPoint.off();

st.stop();
