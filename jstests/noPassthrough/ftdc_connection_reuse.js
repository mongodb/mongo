/**
 * Verify FTDC connection pool stats for connection reuse: connections used just once + cumulative
 * time connections are in-use, per-pool.
 *
 * @tags: [requires_sharding, requires_fcv_63]
 */

load("jstests/libs/fail_point_util.js");
load('jstests/libs/ftdc.js');
load("jstests/libs/parallelTester.js");

(function() {
'use strict';

const ftdcPath = MongoRunner.toRealPath('ftdc');
const st = new ShardingTest({
    shards: {rs0: {nodes: 1}},
    mongos: {
        s0: {setParameter: {diagnosticDataCollectionDirectoryPath: ftdcPath}},
    }
});

const kDbName = jsTestName();
const kCollName = "test";
const kOperations = 5;
const testDB = st.s.getDB(kDbName);
const coll = testDB.getCollection(kCollName);

const numPools = assert.commandWorked(
    st.s.adminCommand({"getParameter": 1, "taskExecutorPoolSize": 1}))["taskExecutorPoolSize"];

function sumOverPools(diagnosticData, host, property) {
    let sum = 0;
    for (const pool of Object.keys(diagnosticData["pools"])) {
        const poolStats = diagnosticData["pools"][pool];
        if (poolStats.hasOwnProperty(host)) {
            sum += poolStats[host][property] || 0;
        }
    }
    return sum;
}

function getDiagnosticData() {
    let stats = verifyGetDiagnosticData(st.s.getDB("admin")).connPoolStats;
    jsTestLog("FTDC stats: " + tojson(stats));
    assert(stats.hasOwnProperty('totalWasUsedOnce'));
    assert(stats.hasOwnProperty('totalConnUsageTimeMillis'));
    return stats;
}

function configureReplSetFailpoint(name, modeValue) {
    st.rs0.nodes.forEach(function(node) {
        assert.commandWorked(node.getDB("admin").runCommand({
            configureFailPoint: name,
            mode: modeValue,
            data: {
                shouldCheckForInterrupt: true,
                nss: kDbName + "." + kCollName,
            },
        }));
    });
}

var threads = [];

function launchFinds({times, readPref, shouldFail}) {
    jsTestLog("Starting " + times + " connections");
    for (var i = 0; i < times; i++) {
        var thread = new Thread(function(connStr, readPref, dbName, shouldFail, collName) {
            var client = new Mongo(connStr);
            const ret = client.getDB(dbName).runCommand(
                {find: collName, limit: 1, "$readPreference": {mode: readPref}});

            if (shouldFail) {
                assert.commandFailed(ret);
            } else {
                assert.commandWorked(ret);
            }
        }, st.s.host, readPref, kDbName, shouldFail, kCollName);
        thread.start();
        threads.push(thread);
    }
}

function resetPools() {
    const cfg = st.rs0.getPrimary().getDB('local').system.replset.findOne();
    const allHosts = cfg.members.map(x => x.host);
    assert.commandWorked(st.s.adminCommand({dropConnections: 1, hostAndPort: allHosts}));
    // FTDC data is collected periodically. Check that the data returned reflects that the pools
    // have been dropped before resuming testing.
    assert.soon(() => {
        const stats = getDiagnosticData();
        // The shard has a single node in its replica set.
        return !stats.hasOwnProperty(allHosts[0]);
    }, "Failed to wait for pool stats to reflect dropped pools");
}

[1, 2, 3].forEach(v => assert.commandWorked(coll.insert({x: v})));
st.rs0.awaitReplication();

// Check that the amount of time connections from the pools are in-use monotonically increases with
// each operation that is run.
let previous = getDiagnosticData()["totalConnUsageTimeMillis"];
let initialVal = previous;
for (let i = 0; i < kOperations; i++) {
    jsTestLog("Issuing find #" + i);
    assert.commandWorked(testDB.runCommand({"find": kCollName}));
    assert.soon(() => {
        let poolStats = getDiagnosticData();
        return poolStats["totalConnUsageTimeMillis"] >= previous;
    }, "totalConnUsageTime failed to update within time limit", 10 * 1000);
    let res = getDiagnosticData()["totalConnUsageTimeMillis"];
    previous = res;
}
assert.gte(getDiagnosticData()["totalConnUsageTimeMillis"],
           initialVal,
           "totalConnUsageTimeMillis failed to increase after issuing find operations");

resetPools();

assert.commandWorked(st.s.adminCommand({
    "setParameter": 1,
    ShardingTaskExecutorPoolMinSize: 3,
    ShardingTaskExecutorPoolRefreshRequirementMS: 1000
}));

// Launch 3 blocked finds per pool and verify that all are in-use.
jsTestLog("Launching blocked finds");
const fpRs =
    configureFailPointForRS(st.rs0.nodes,
                            "waitInFindBeforeMakingBatch",
                            {shouldCheckForInterrupt: true, nss: kDbName + "." + kCollName});
const numFinds = 3 * numPools;
launchFinds({times: numFinds, readPref: "primary"});
assert.soon(() => {
    let poolStats = getDiagnosticData();
    return poolStats["totalInUse"] == numFinds;
}, "Launched finds failed to be marked as inUse within time limit", 10 * 1000);

// Unblock finds and ensure that the connections are no longer in-use.
jsTestLog("Unblocking finds");
fpRs.off();

assert.soon(() => {
    return getDiagnosticData()["totalInUse"] == 0;
}, "In use connections failed to be drop to 0 within time limit", 20 * 1000);

// Reduce pool size to verify that dropped connections were marked as having been used only once.
jsTestLog("Reducing pool size");
assert.commandWorked(st.s.adminCommand(
    {"setParameter": 1, ShardingTaskExecutorPoolMinSize: 1, ShardingTaskExecutorPoolMaxSize: 1}));

assert.soon(() => {
    // Other connections (e.g. connection to config primary running {find: "shards"}) may be
    // dropped as a result of the max connections restriction, so it's possible more than 2
    // connections are dropped per pool.
    return getDiagnosticData()["totalWasUsedOnce"] >= 2 * numPools;
}, "Dropped connections failed to be marked as wasUsedOnce within time limit", 20 * 1000);

threads.forEach(function(thread) {
    thread.join();
});
st.stop();
})();
