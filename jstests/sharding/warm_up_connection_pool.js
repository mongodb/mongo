/**
 * This test checks that Mongos is pre-warming connections before accepting connections.
 *
 * This test requires users to persist across a restart.
 * @tags: [requires_persistence]
 */

(function() {
'use strict';
load("jstests/replsets/rslib.js");

function runTest(setParams, assertCheck, extraOptionsAction) {
    jsTestLog('Starting next iteration');
    const test = new ShardingTest({shards: 2, mongosOptions: setParams});
    var db = test.getDB("test");

    const shardCommand = {shardcollection: "test.foo", key: {num: 1}};

    // shard
    assert.commandWorked(test.s0.adminCommand({enablesharding: "test"}));
    assert.commandWorked(test.s0.adminCommand(shardCommand));

    var primary;
    var mId;
    if (extraOptionsAction !== undefined) {
        const resp = extraOptionsAction(test);
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

    if (extraOptionsAction !== undefined) {
        jsTestLog(`Restart ${mId}`);
        test.rs0.restart(mId);
        // Wait for mongos to recognize that the host is up.
        awaitRSClientHosts(test.s0, test.rs0.nodes[mId], {ok: true});
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
                    1,
                `Connections ${tojson(connPoolStats)}`,
                5 * 60 * 1000,
                1000);
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

// Tests establishes more connections when parameter is set
var biggerPoolSizeParams = {setParameter: {ShardingTaskExecutorPoolMinSize: 3}};
var biggerPoolSizeAssertCheck = function(connPoolStats, currentShard) {
    assert.soon(() => connPoolStats["hosts"][currentShard]["inUse"] +
                        connPoolStats["hosts"][currentShard]["available"] +
                        connPoolStats["hosts"][currentShard]["refreshing"] >=
                    3,
                `Connections ${tojson(connPoolStats)}`,
                5 * 60 * 1000,
                1000);
};

runTest(biggerPoolSizeParams, biggerPoolSizeAssertCheck);

// Tests establishes connections it can and ignores the ones it can't
var shutdownNodeParams = {};
var shutdownNodeAssertCheck = function(connPoolStats, currentShard, primary) {
    if (currentShard == primary) {
        assert.soon(() => connPoolStats["hosts"][currentShard]["inUse"] +
                            connPoolStats["hosts"][currentShard]["available"] ===
                        0,
                    `Connections ${tojson(connPoolStats)}`,
                    5 * 60 * 1000,
                    1000);
    } else {
        assert.soon(() => connPoolStats["hosts"][currentShard]["inUse"] +
                            connPoolStats["hosts"][currentShard]["available"] +
                            connPoolStats["hosts"][currentShard]["refreshing"] >=
                        1,
                    `Connections ${tojson(connPoolStats)}`,
                    5 * 60 * 1000,
                    1000);
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
