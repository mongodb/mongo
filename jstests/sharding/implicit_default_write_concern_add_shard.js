/**
 * Tests adding shard to sharded cluster will fail if the implicitDefaultWriteConcern is
 * w:1 and CWWC is not set.
 */

(function() {
"use strict";

// Adds a shard near the end of the test that won't have metadata for the sessions collection during
// test shutdown. This is only a problem with a config shard because otherwise there are no shards
// so the sessions collection can't be created.
TestData.skipCheckShardFilteringMetadata = TestData.configShard;

load("jstests/replsets/rslib.js");  // For reconfig and isConfigCommitted.

function addNonArbiterNode(nodeId, rst) {
    const config = rst.getReplSetConfigFromNode();
    config.members.push({_id: nodeId, host: rst.add().host});
    reconfig(rst, config);
    assert.soon(() => isConfigCommitted(rst.getPrimary()));
    rst.waitForConfigReplication(rst.getPrimary());
    rst.awaitReplication();
    // When we add a new node to a replica set, we temporarily add the "newlyAdded" field so that it
    // is non-voting until it completes initial sync.
    // This waits for the primary to see that the node has transitioned to a secondary, recovering,
    // or rollback state to ensure that we can do the automatic reconfig to remove the "newlyAdded"
    // field so that the node can actually vote so replication coordinator can update implicit
    // default write-concern depending on the newly added voting member.
    rst.waitForAllNewlyAddedRemovals();
}

function testAddShard(CWWCSet, isPSASet, fixAddShard) {
    jsTestLog("Running sharding test with CWWCSet: " + tojson(CWWCSet) +
              ", isPSASet: " + tojson(isPSASet));
    let replSetNodes = [{}, {}];
    if (isPSASet) {
        replSetNodes = [{}, {}, {arbiter: true}];
    }

    let shardServer = new ReplSetTest(
        {name: "shardServer", nodes: replSetNodes, nodeOptions: {shardsvr: ""}, useHostName: true});
    const conns = shardServer.startSet();
    shardServer.initiate();

    const st = new ShardingTest({
        shards: TestData.configShard ? 1 : 0,
        mongos: 1,
    });
    var admin = st.getDB('admin');

    if (CWWCSet) {
        jsTestLog("Setting the CWWC before adding shard.");
        assert.commandWorked(st.s.adminCommand(
            {setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority", wtimeout: 0}}));
    }

    jsTestLog("Attempting to add shard to the cluster");
    if (!CWWCSet && isPSASet) {
        jsTestLog("Adding shard to the cluster should fail.");
        assert.commandFailed(admin.runCommand({addshard: shardServer.getURL()}));

        if (fixAddShard == "setCWWC") {
            jsTestLog("Setting the CWWC to fix addShard.");
            assert.commandWorked(st.s.adminCommand(
                {setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority", wtimeout: 0}}));
        } else {
            jsTestLog("Reconfig shardServer to fix addShard.");
            addNonArbiterNode(3, shardServer);
            addNonArbiterNode(4, shardServer);
        }
    }

    jsTestLog("Adding shard to the cluster should succeed.");
    assert.commandWorked(admin.runCommand({addshard: shardServer.getURL()}));

    st.stop();
    shardServer.stopSet();
}

for (const CWWCSet of [true, false]) {
    for (const isPSASet of [false, true]) {
        if (!CWWCSet && isPSASet) {
            for (const fixAddShard of ["setCWWC", "reconfig"]) {
                testAddShard(CWWCSet, isPSASet, fixAddShard);
            }
        } else {
            testAddShard(CWWCSet, isPSASet);
        }
    }
}
})();
