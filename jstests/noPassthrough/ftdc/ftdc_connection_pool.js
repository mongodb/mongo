/**
 * The FTDC connection pool stats from mongos are a different structure than the connPoolStats
 * command, verify its contents.
 *
 * @tags: [requires_sharding, requires_fcv_53]
 */
import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const testPath = MongoRunner.toRealPath("ftdc_dir");
const st = new ShardingTest({
    shards: 2,
    mongos: {
        s0: {setParameter: {diagnosticDataCollectionDirectoryPath: testPath}},
    },
});

const admin = st.s0.getDB("admin");
const stats = verifyGetDiagnosticData(admin).connPoolStats;
jsTestLog(`Diagnostic connection pool stats: ${tojson(stats)}`);

assert(stats.hasOwnProperty("totalInUse"));
assert(stats.hasOwnProperty("totalAvailable"));
assert(stats.hasOwnProperty("totalCreated"));
assert(stats.hasOwnProperty("totalRefreshing"));
assert(stats.hasOwnProperty("totalRefreshed"));
assert("hello" in stats["replicaSetMonitor"]);
const helloStats = stats["replicaSetMonitor"]["hello"];
assert(helloStats.hasOwnProperty("currentlyActive"));
assert("getHostAndRefresh" in stats["replicaSetMonitor"]);
const getHostStats = stats["replicaSetMonitor"]["getHostAndRefresh"];
assert(getHostStats.hasOwnProperty("currentlyActive"));

// The connPoolStats command reply has "hosts", but FTDC's stats do not.
assert(!stats.hasOwnProperty("hosts"));

// Check a few properties, without attempting to be thorough.
assert(stats.pools.hasOwnProperty("NetworkInterfaceTL-Sharding-Fixed"));
assert(stats.replicaSetPingTimesMillis.hasOwnProperty(st.configRS.name));

st.stop();
