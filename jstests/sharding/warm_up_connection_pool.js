/*
 * @tags: [requires_fcv_44, multiversion_incompatible]
 */

// Checking UUID and index consistency involves talking to a shard node, which in this
// test is shutdown.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

(function() {
'use strict';

function runTest(setParams, assertCheck, extraOptions) {
    const test = new ShardingTest({shards: 2, mongosOptions: setParams});
    var db = test.getDB("test");

    const shardCommand = {shardcollection: "test.foo", key: {num: 1}};

    // shard
    assert.commandWorked(test.s0.adminCommand({enablesharding: "test"}));
    assert.commandWorked(test.s0.adminCommand(shardCommand));

    var primary;
    var mId;
    if (extraOptions !== undefined) {
        const resp = extraOptions(test);
        primary = resp.connString;
        mId = resp.nodeId;
    }

    test.restartMongos(0);

    db = test.getDB("admin");
    var connPoolStats = db.runCommand({connPoolStats: 1});
    var shardList = db.runCommand({listShards: 1});

    for (var shard in shardList["shards"]) {
        var currentShard = getShardHostName(shardList, shard);
        assertCheck(connPoolStats, currentShard, primary);
    }

    if (extraOptions !== undefined) {
        test.rs0.restart(mId);
    }

    test.stop();
}

function getShardHostName(shardlist, shard) {
    return shardlist["shards"][shard]["host"].split("/")[1];
}

// Tests basic warm up of connection pool
var testWarmUpParams = {};
var testWarmUpAssertCheck = function(connPoolStats, currentShard) {
    assert.soon(() => connPoolStats["hosts"][currentShard]["inUse"] +
                        connPoolStats["hosts"][currentShard]["available"] +
                        connPoolStats["hosts"][currentShard]["refreshing"] >=
                    1);
};

runTest(testWarmUpParams, testWarmUpAssertCheck);

// Tests does not warm up connection pool when parameter is disabled
var warmUpDisabledParams = {
    setParameter: {warmMinConnectionsInShardingTaskExecutorPoolOnStartup: false}
};
var warmUpDisabledAssertCheck = function(connPoolStats, currentShard) {
    assert.eq(null, connPoolStats["hosts"][currentShard]);
};

runTest(warmUpDisabledParams, warmUpDisabledAssertCheck);

// Tests establishes more connections when parameter is set. Increase the amount
// of time to establish more connections to avoid timing out before establishing
// the expected number of connections.
var biggerPoolSizeParams = {
    setParameter: {
        ShardingTaskExecutorPoolMinSize: 3,
        warmMinConnectionsInShardingTaskExecutorPoolOnStartupWaitMS: 60000
    }
};
var biggerPoolSizeAssertCheck = function(connPoolStats, currentShard) {
    assert.soon(() => connPoolStats["hosts"][currentShard]["inUse"] +
                        connPoolStats["hosts"][currentShard]["available"] +
                        connPoolStats["hosts"][currentShard]["refreshing"] >=
                    3);
};

runTest(biggerPoolSizeParams, biggerPoolSizeAssertCheck);

// Tests establishes connections it can and ignores the ones it can't
var shutdownNodeParams = {};
var shutdownNodeAssertCheck = function(connPoolStats, currentShard, primary) {
    if (currentShard == primary) {
        assert.soon(() => connPoolStats["hosts"][currentShard]["inUse"] +
                            connPoolStats["hosts"][currentShard]["available"] +
                            connPoolStats["hosts"][currentShard]["refreshing"] ===
                        0);
    } else {
        assert.soon(() => connPoolStats["hosts"][currentShard]["inUse"] +
                            connPoolStats["hosts"][currentShard]["available"] +
                            connPoolStats["hosts"][currentShard]["refreshing"] ===
                        1);
    }
};
var shutdownNodeExtraOptions = function(test) {
    const nodeList = test.rs0.nodeList();
    const master = test.rs0.getPrimary();
    var mId = test.rs0.getNodeId(master);

    test.rs0.stop(mId);
    return {connString: nodeList[mId], nodeId: mId};
};

runTest(shutdownNodeParams, shutdownNodeAssertCheck, shutdownNodeExtraOptions);
})();