/**
 * Verify FTDC connection pool stats for connection reuse: connections used just once + cumulative
 * time connections are in-use, per-pool.
 *
 * @tags: [requires_sharding, requires_fcv_62, featureFlagConnHealthMetrics]
 */

load("jstests/libs/fail_point_util.js");
load('jstests/libs/ftdc.js');
load("jstests/libs/parallelTester.js");

(function() {
'use strict';

const ftdcPath = MongoRunner.toRealPath('ftdc');
const st = new ShardingTest({
    shards: 1,
    mongos: {
        s0: {setParameter: {diagnosticDataCollectionDirectoryPath: ftdcPath}},
    }
});

const kDbName = jsTestName();
const kCollName = "test";
const kOperations = 5;
const testDB = st.s.getDB(kDbName);
const coll = testDB.getCollection(kCollName);

function getDiagnosticData() {
    assert.soon(() => {
        let stats = verifyGetDiagnosticData(st.s.getDB("admin")).connPoolStats;
        return stats["pools"].hasOwnProperty('NetworkInterfaceTL-TaskExecutorPool-0');
    }, "Failed to load NetworkInterfaceTL-TaskExecutorPool-0 in FTDC within time limit");
    const stats = verifyGetDiagnosticData(st.s.getDB("admin")).connPoolStats;
    assert(stats.hasOwnProperty('totalWasUsedOnce'));
    assert(stats.hasOwnProperty('totalConnUsageTimeMillis'));
    return stats["pools"]["NetworkInterfaceTL-TaskExecutorPool-0"];
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
}

[1, 2, 3].forEach(v => assert.commandWorked(coll.insert({x: v})));
st.rs0.awaitReplication();

// Check that the amount of time connections from the pool are in-use monotonically increases with
// each operation that is run.
let previous = getDiagnosticData()["poolConnUsageTimeMillis"];
let initialVal = previous;
for (let i = 0; i < kOperations; i++) {
    jsTestLog("Issuing find #" + i);
    assert.commandWorked(testDB.runCommand({"find": kCollName}));
    assert.soon(() => {
        let poolStats = getDiagnosticData();
        return poolStats["poolConnUsageTimeMillis"] >= previous;
    }, "poolConnUsageTime failed to update within time limit", 10 * 1000);
    let res = getDiagnosticData()["poolConnUsageTimeMillis"];
    previous = res;
}
assert.gt(getDiagnosticData()["poolConnUsageTimeMillis"],
          initialVal,
          "poolConnUsageTimeMillis failed to increase after issuing find operations");

resetPools();

assert.commandWorked(st.s.adminCommand({
    "setParameter": 1,
    ShardingTaskExecutorPoolMinSize: 3,
    ShardingTaskExecutorPoolRefreshRequirementMS: 1000
}));

// Launch 3 blocked finds and verify that all 3 are in-use.
jsTestLog("Launching blocked finds");
configureReplSetFailpoint("waitInFindBeforeMakingBatch", "alwaysOn");
launchFinds({times: 3, readPref: "primary"});
assert.soon(() => {
    let poolStats = getDiagnosticData();
    return poolStats["poolInUse"] == 3;
}, "Launched finds failed to be marked as inUse within time limit", 10 * 1000);

// Unblock finds, and reduce pool size to verify that dropped connections were marked as having been
// used only once and remaining connections are no longer active.
jsTestLog("Unblocking finds, reducing pool size");
configureReplSetFailpoint("waitInFindBeforeMakingBatch", "off");
assert.commandWorked(st.s.adminCommand(
    {"setParameter": 1, ShardingTaskExecutorPoolMinSize: 1, ShardingTaskExecutorPoolMaxSize: 1}));
assert.soon(() => {
    let poolStats = getDiagnosticData();
    return poolStats["poolInUse"] == 0 && poolStats["poolWasUsedOnce"] == 2;
}, "Dropped connections failed to be marked as wasUsedOnce within time limit", 20 * 1000);

threads.forEach(function(thread) {
    thread.join();
});
st.stop();
})();
