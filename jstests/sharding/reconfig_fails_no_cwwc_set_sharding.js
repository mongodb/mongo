/*
 * Test that a reconfig for a shard that would change the implicit default write concern to w:1
 * fails if CWWC is not set.
 *
 * Temporary config shard incompatible because it hits a sharding metadata hook failure on cluster
 * shutdown.
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_fcv_51,
 * ]
 */

(function() {
'use strict';

// TODO SERVER-75820: Investigate why a shard node doesn't have metadata at test shutdown.
TestData.skipCheckShardFilteringMetadata = true;

load("jstests/replsets/rslib.js");  // For reconfig, isConfigCommitted and
                                    // safeReconfigShouldFail.

const configurationIncompatibleMsg =
    "config that would change the implicit default write concern on the shard to {w: 1}.";

const configServerUnreachableMsg = "the request to the config server is failing with error:";

let addNodeConfig = function(rst, nodeId, conn, arbiter) {
    const config = rst.getReplSetConfigFromNode();
    if (arbiter) {
        config.members.push({_id: nodeId, host: conn.host, arbiterOnly: true});
    } else {
        config.members.push({_id: nodeId, host: conn.host});
    }

    return config;
};

let removeNodeConfig = function(rst, conn) {
    const config = rst.getReplSetConfigFromNode();
    for (var i = 0; i < config.members.length; i++) {
        if (config.members[i].host == conn.host) {
            config.members.splice(i, 1);
            break;
        }
    }

    return config;
};

function testReconfig(rst, config, shouldSucceed, errCode, errMsg) {
    if (shouldSucceed) {
        reconfig(rst, config);
        assert.soon(() => isConfigCommitted(rst.getPrimary()));
        rst.waitForConfigReplication(rst.getPrimary());
        rst.awaitReplication();
    } else {
        safeReconfigShouldFail(rst, config, false /* force */, errCode, errMsg);

        // A force reconfig should also fail.
        safeReconfigShouldFail(rst, config, true /* force */, errCode, errMsg);
    }
}

jsTestLog("Testing to reconfig a shard that is not yet added to a sharded cluster.");
let logPrefix = "While the shard is not part of a sharded cluster: ";
let shardServer = new ReplSetTest(
    {name: "shardServer", nodes: 1, nodeOptions: {shardsvr: ""}, useHostName: true});
shardServer.startSet();
shardServer.initiateWithHighElectionTimeout();

jsTestLog(logPrefix + "Adding an arbiter node that will change IDWC to (w:1) should succeed.");
let arbiter = shardServer.add();
testReconfig(shardServer,
             addNodeConfig(shardServer, 1 /* nodeId */, arbiter /* conn */, true /* arbiter */),
             true /* shouldSucceed */);

jsTestLog(logPrefix +
          "Removing an arbiter node that will change IDWC to (w: 'Majority') should succeed.");
shardServer.remove(arbiter);
testReconfig(shardServer, removeNodeConfig(shardServer, arbiter), true /* shouldSucceed */);

shardServer.stopSet();

jsTestLog("Testing to reconfig a shard that is part of a sharded cluster.");
logPrefix = "While the shard is part of a sharded cluster: ";
shardServer = new ReplSetTest(
    {name: "shardServer", nodes: 1, nodeOptions: {shardsvr: ""}, useHostName: true});
shardServer.startSet();
shardServer.initiateWithHighElectionTimeout();

const st = new ShardingTest({shards: TestData.configShard ? 1 : 0, mongos: 1});
var admin = st.getDB('admin');

jsTestLog("Adding the shard to the cluster should succeed.");
assert.commandWorked(admin.runCommand({addshard: shardServer.getURL()}));

jsTestLog(logPrefix +
          "Adding an non-arbiter node that will keep IDWC set to (w: 'Majority') should succeed.");
testReconfig(
    shardServer,
    addNodeConfig(shardServer, 1 /* nodeId */, shardServer.add() /* conn */, false /* arbiter */),
    true /* shouldSucceed */);

jsTestLog(logPrefix + "Adding an arbiter node that will change IDWC to (w:1) should fail.");
arbiter = shardServer.add();
testReconfig(shardServer,
             addNodeConfig(shardServer, 2 /* nodeId */, arbiter /* conn */, true /* arbiter */),
             false /* shouldSucceed */,
             ErrorCodes.NewReplicaSetConfigurationIncompatible,
             configurationIncompatibleMsg);

jsTestLog("Setting the CWWC on the cluster.");
logPrefix = "While CWWC is set on the sharded cluster: ";
assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority", wtimeout: 0}}));

jsTestLog(logPrefix + "Adding an arbiter node that will change IDWC to (w:1) should succeed.");
testReconfig(shardServer,
             addNodeConfig(shardServer, 2 /* nodeId */, arbiter /* conn */, true /* arbiter */),
             true /* shouldSucceed */);

jsTestLog("Testing when the config server is unreachable.");
logPrefix = "While config server is unreachable: ";
jsTestLog("Shutting down all config servers");
for (var i = 0; i < st.configRS.nodes.length; i++) {
    st.stopConfigServer(i);
}
st.configRS.awaitNoPrimary();

// The reason why this is okay is because CWWC had to have been set for the cluster to get to this
// point, even if we can't check the config servers for it.
jsTestLog(logPrefix +
          "Removing an arbiter node that will change IDWC to (w: 'Majority') should succeed.");
shardServer.remove(arbiter);
testReconfig(shardServer, removeNodeConfig(shardServer, arbiter), true /* shouldSucceed */);

jsTestLog(logPrefix + "Adding an arbiter node that will change IDWC to (w: 1) should fail.");
testReconfig(shardServer,
             addNodeConfig(shardServer, 2 /* nodeId */, arbiter /* conn */, true /* arbiter */),
             false /* shouldSucceed */,
             ErrorCodes.ConfigServerUnreachable,
             configServerUnreachableMsg);

jsTestLog("Restarting the config servers.");
logPrefix = "While the config server is reachable: ";
for (var i = 0; i < st.configRS.nodes.length; i++) {
    st.restartConfigServer(i, undefined, undefined, true /* wait */);
}
st.configRS.awaitNodesAgreeOnPrimary();
print("Sleeping for 60 seconds to let the other shards restart their ReplicaSetMonitors");
sleep(60000);

jsTestLog(logPrefix + "Adding an arbiter node will change IDWC to (w:1) should succeed.");
arbiter = shardServer.add();
testReconfig(shardServer,
             addNodeConfig(shardServer, 2 /* nodeId */, arbiter /* conn */, true /* arbiter */),
             true /* shouldSucceed */);

jsTestLog("Stopping the cluster.");
st.stop();

jsTestLog("Stopping the shard.");
shardServer.stopSet();
})();
