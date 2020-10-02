/**
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */

// Checking UUID and index consistency involves talking to a shard node, which in this
// test is shutdown.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;

(function() {
'use strict';

function runTest(setParams, connPoolStatsCheck, extraOptions) {
    const test = new ShardingTest({shards: 2, mongosOptions: setParams});
    var db = test.getDB("test");

    const shardCommand = {shardcollection: "test.foo", key: {num: 1}};

    // shard
    assert.commandWorked(test.s0.adminCommand({enablesharding: "test"}));
    assert.commandWorked(test.s0.adminCommand(shardCommand));

    var primary;
    var pId;
    if (extraOptions !== undefined) {
        const resp = extraOptions(test);
        primary = resp.connString;
        pId = resp.nodeId;
    }

    test.restartMongos(0);

    db = test.getDB("admin");
    const shardDocs = db.runCommand({listShards: 1})["shards"];

    assert.soon(() => {
        let connPoolStats = db.runCommand({connPoolStats: 1});
        for (const shardDoc of shardDocs) {
            let currentShard = shardDoc["host"].split("/")[1];
            if (!connPoolStatsCheck(connPoolStats, currentShard, primary)) {
                return false;
            }
        }
        return true;
    });

    if (extraOptions !== undefined) {
        test.rs0.restart(pId);
    }

    test.stop();
}

jsTest.log("Tests basic warm up of connection pool");
var testWarmUpParams = {};
var testWarmUpConnPoolStatsCheck = function(connPoolStats, currentShard) {
    return connPoolStats["hosts"][currentShard]["inUse"] +
        connPoolStats["hosts"][currentShard]["available"] +
        connPoolStats["hosts"][currentShard]["refreshing"] >=
        1;
};

runTest(testWarmUpParams, testWarmUpConnPoolStatsCheck);

jsTest.log("Tests does not warm up connection pool when parameter is disabled");
var warmUpDisabledParams = {
    setParameter: {warmMinConnectionsInShardingTaskExecutorPoolOnStartup: false}
};
var warmUpDisabledConnPoolStatsCheck = function(connPoolStats, currentShard) {
    return undefined === connPoolStats["hosts"][currentShard];
};

runTest(warmUpDisabledParams, warmUpDisabledConnPoolStatsCheck);

jsTest.log("Tests establishes more connections when parameter is set.");
// Increase the amount of time to establish more connections to avoid timing out
// before establishing the expected number of connections.
var biggerPoolSizeParams = {
    setParameter: {
        ShardingTaskExecutorPoolMinSize: 3,
        warmMinConnectionsInShardingTaskExecutorPoolOnStartupWaitMS: 60000
    }
};
var biggerPoolSizeConnPoolStatsCheck = function(connPoolStats, currentShard) {
    return connPoolStats["hosts"][currentShard]["inUse"] +
        connPoolStats["hosts"][currentShard]["available"] +
        connPoolStats["hosts"][currentShard]["refreshing"] >=
        3;
};

runTest(biggerPoolSizeParams, biggerPoolSizeConnPoolStatsCheck);

jsTest.log("Tests establishes connections it can and ignores the ones it can't");
var shutdownNodeParams = {};
var shutdownNodeConnPoolStatsCheck = function(connPoolStats, currentShard, primary) {
    if (currentShard == primary) {
        return connPoolStats["hosts"][currentShard]["inUse"] +
            connPoolStats["hosts"][currentShard]["available"] +
            connPoolStats["hosts"][currentShard]["refreshing"] ===
            0;
    }
    return connPoolStats["hosts"][currentShard]["inUse"] +
        connPoolStats["hosts"][currentShard]["available"] +
        connPoolStats["hosts"][currentShard]["refreshing"] ===
        1;
};
var shutdownNodeExtraOptions = function(test) {
    const nodeList = test.rs0.nodeList();
    const primary = test.rs0.getPrimary();
    var pId = test.rs0.getNodeId(primary);

    test.rs0.stop(pId);
    return {connString: nodeList[pId], nodeId: pId};
};

runTest(shutdownNodeParams, shutdownNodeConnPoolStatsCheck, shutdownNodeExtraOptions);
})();
