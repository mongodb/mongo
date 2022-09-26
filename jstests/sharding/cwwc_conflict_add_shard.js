/**
 * Tests adding shard to sharded cluster will fail if CWWC on shard disagrees with existing CWWC on
 * cluster.
 * @tags: [
 *    requires_majority_read_concern,
 *    requires_persistence,
 *    does_not_support_stepdowns,
 * ]
 */

(function() {
"use strict";
load('jstests/sharding/libs/remove_shard_util.js');

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

const st = new ShardingTest({shards: 1, mongos: 1});
const admin = st.getDB('admin');
let shardServer;

function createNewShard() {
    jsTest.log("Creating new shard");
    let start = new Date();
    if (shardServer) {
        shardServer.stopSet({skipValidation: true});
        jsTest.log("Stop set took: " + (new Date() - start));
    }

    shardServer = new ReplSetTest({name: "shardServer", nodes: 1, useHostName: true});
    shardServer.startSet();
    shardServer.initiate();
    jsTest.log("Create new shard took: " + (new Date() - start));
}

function convertShardToRS() {
    jsTestLog("Converting shard server to replicaSet.");
    let start = new Date();
    shardServer.nodes.forEach(function(node) {
        delete node.fullOptions.shardsvr;
    });

    shardServer.restart(shardServer.nodes, {skipValidation: true});
    jsTest.log("Coversion shard -> RS took: " + (new Date() - start));
}

function convertRSToShard() {
    let start = new Date();
    jsTestLog("Converting replicaSet server to shardServer.");
    shardServer.restart(shardServer.nodes, {shardsvr: "", skipValidation: true});
    jsTest.log("Coversion RS -> Shard took: " + (new Date() - start));
}

function testAddShard(cwwcOnShard, cwwcOnCluster, shouldSucceed, fixCWWCOnShard) {
    jsTestLog("Running addShard test with CWWCOnShard: " + tojson(cwwcOnShard) +
              " and CWWCOnCluster: " + tojson(cwwcOnCluster));

    let cwwcOnShardCmd, cwwcOnClusterCmd;

    if (cwwcOnShard) {
        jsTestLog("Setting the CWWC on shard before adding it.");
        cwwcOnShardCmd = {
            setDefaultRWConcern: 1,
            defaultWriteConcern: cwwcOnShard,
            writeConcern: {w: "majority"}
        };
        assert.commandWorked(shardServer.getPrimary().adminCommand(cwwcOnShardCmd));
    }

    if (cwwcOnCluster) {
        jsTestLog("Setting the CWWC on cluster before adding shard.");
        cwwcOnClusterCmd = {
            setDefaultRWConcern: 1,
            defaultWriteConcern: cwwcOnCluster,
            writeConcern: {w: "majority"}
        };
        assert.commandWorked(st.s.adminCommand(cwwcOnClusterCmd));
    }

    jsTestLog("Attempting to add shard to the cluster");
    convertRSToShard();

    if (!shouldSucceed) {
        jsTestLog("Adding shard to the cluster should fail.");
        assert.commandFailed(admin.runCommand({addshard: shardServer.getURL()}));

        if (fixCWWCOnShard) {
            convertShardToRS();
            jsTestLog("Setting the CWWC on shard to match CWWC on cluster.");
            assert.commandWorked(shardServer.getPrimary().adminCommand(cwwcOnClusterCmd));
            convertRSToShard();
        } else {
            jsTestLog("Setting the CWWC on cluster to match CWWC on shard.");
            assert.commandWorked(st.s.adminCommand(cwwcOnShardCmd));
        }
    }

    jsTestLog("Adding shard to the cluster should succeed.");
    assert.commandWorked(admin.runCommand({addshard: shardServer.getURL()}));

    // Cleanup.
    let start = new Date();
    jsTestLog("Removing the shard from the cluster should succeed.");
    removeShard(st, shardServer.getURL());
    jsTestLog("Shard removal completed. took: " + (new Date() - start));
    convertShardToRS();
}

const cwwc = [
    {w: 1},
    {w: "majority"},
    {w: "majority", wtimeout: 0, j: false},
    {w: "majority", wtimeout: 0, j: true}
];

createNewShard();
// No CWWC set neither on shard nor cluster should succeed.
testAddShard(undefined /* cwwcOnShard */, undefined /* cwwcOnCluster */, true /* shouldSucceed */);

// No CWWC set on cluster while shard has CWWC should fail.
testAddShard(cwwc[0] /* cwwcOnShard */,
             undefined /* cwwcOnCluster */,
             false /* shouldSucceed */,
             false /* fixCWWCOnShard */);

createNewShard();
// No CWWC set on shard while cluster has CWWC should succeed.
testAddShard(undefined /* cwwcOnShard */, cwwc[0] /* cwwcOnCluster */, true /* shouldSucceed */);

for (var i = 0; i < cwwc.length; i++) {
    // Setting the same CWWC on shard and cluster should succeed.
    testAddShard(cwwc[i] /* cwwcOnShard */, cwwc[i] /* cwwcOnCluster */, true /* shouldSucceed */);
    for (var j = i + 1; j < cwwc.length; j++) {
        for (const fixCWWCOnShard of [true, false]) {
            // Setting different CWWC on shard and cluster should fail.
            testAddShard(cwwc[i] /* cwwcOnShard */,
                         cwwc[j] /* cwwcOnCluster */,
                         false /* shouldSucceed */,
                         fixCWWCOnShard);
        }
    }
}

st.stop();
shardServer.stopSet({skipValidation: true});
})();
