/**
 * Tests that the cluster parameter "shardedClusterCardinalityForDirectConns" has the correct value
 * after the addShard command is interrupted before the cluster parameter is updated but the
 * addShard command is retried after that.
 *
 * Additionally check that data movement is properly disallowed while the cluster parameter has yet
 * to be updated by the addShard command.
 *
 * @tags: [
 *   requires_fcv_80
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    checkClusterParameter,
    interruptConfigsvrAddShard
} from "jstests/sharding/libs/cluster_cardinality_parameter_util.js";

const st = new ShardingTest({shards: 1});

// Create an unsharded and a sharded collection to be used later on in the test.
const dbName = 'test';
const unshardedCollName = 'unsharded';
const shardedCollName1 = 'sharded1';
const shardedCollName2 = 'sharded2';
const shardedCollName3 = 'sharded3';
assert.commandWorked(st.s.getDB(dbName).createCollection(unshardedCollName));
assert.commandWorked(
    st.s.adminCommand({shardCollection: dbName + '.' + shardedCollName1, key: {_id: 1}}));
assert.commandWorked(
    st.s.adminCommand({shardCollection: dbName + '.' + shardedCollName2, key: {_id: 1}}));
assert.commandWorked(
    st.s.adminCommand({shardCollection: dbName + '.' + shardedCollName3, key: {_id: 1}}));

jsTest.log("Checking the cluster parameter while the cluster contains one shard");
// There is only one shard in the cluster, so the cluster parameter should be false.
checkClusterParameter(st.configRS, false);
checkClusterParameter(st.rs0, false);

function addShard(mongosHost, shardURL, shardName) {
    const mongos = new Mongo(mongosHost);

    jsTest.log("Start adding shard " + tojsononeline({shardURL, shardName}));
    const res = mongos.adminCommand({addShard: shardURL, name: shardName});
    jsTest.log("Finished adding shard " + tojsononeline({shardURL, shardName}));
    return res;
}

const shard1Name = "shard1";
const shard1Rst = new ReplSetTest({name: shard1Name, nodes: 1});
shard1Rst.startSet({shardsvr: ""});
shard1Rst.initiate();

jsTest.log(
    "Run an addShard command but interrupt it before it updates the cluster cardinality parameter");
const configPrimary = st.configRS.getPrimary();
const fp =
    configureFailPoint(configPrimary, "hangAddShardBeforeUpdatingClusterCardinalityParameter");
const addShardThread = new Thread(addShard, st.s.host, shard1Rst.getURL(), shard1Name);
addShardThread.start();
fp.wait();
interruptConfigsvrAddShard(configPrimary);
assert.commandFailedWithCode(addShardThread.returnData(), ErrorCodes.Interrupted);
fp.off();

jsTest.log("Checking the cluster parameter");
// The addShard command has not set the cluster parameter to true again because of the interrupt.
checkClusterParameter(st.configRS, false);
checkClusterParameter(st.rs0, false);
checkClusterParameter(shard1Rst, false);

jsTest.log("Ensure that data cannot be moved yet even though the second shard is visible");
assert.commandFailedWithCode(
    st.s.adminCommand({moveCollection: dbName + "." + unshardedCollName, toShard: shard1Rst.name}),
    ErrorCodes.IllegalOperation);

// TODO (SERVER-90269) Enable these tests in multiversion once this has been backported to 8.0.
const isMultiversion =
    jsTest.options().shardMixedBinVersions || jsTest.options().useRandomBinVersionsWithinReplicaSet;
if (!isMultiversion) {
    assert.commandFailedWithCode(
        st.s.adminCommand(
            {moveChunk: dbName + "." + shardedCollName1, find: {_id: 0}, to: shard1Rst.name}),
        ErrorCodes.IllegalOperation);
    assert.commandFailedWithCode(st.s.adminCommand({movePrimary: dbName, to: shard1Rst.name}),
                                 ErrorCodes.IllegalOperation);
    // The collection should be able to be resharded locally
    assert.commandWorked(st.s.adminCommand({
        reshardCollection: dbName + '.' + shardedCollName2,
        key: {_id: 1},
        forceRedistribution: true,
        shardDistribution: [{shard: st.shard0.shardName, min: {_id: MinKey}, max: {_id: MaxKey}}]
    }));
    // Resharding to another shard should be disallowed
    assert.commandFailedWithCode(st.s.adminCommand({
        reshardCollection: dbName + '.' + shardedCollName2,
        key: {_id: 1},
        forceRedistribution: true,
        shardDistribution: [{shard: shard1Rst.name, min: {_id: MinKey}, max: {_id: MaxKey}}]
    }),
                                 ErrorCodes.IllegalOperation);
    // The collection should be able to be unsharded locally
    assert.commandWorked(st.s.adminCommand(
        {unshardCollection: dbName + '.' + shardedCollName3, toShard: st.shard0.shardName}));
    assert.commandWorked(
        st.s.adminCommand({shardCollection: dbName + '.' + shardedCollName3, key: {_id: 1}}));
    // Unsharded to another shard should be disallowed
    assert.commandFailedWithCode(
        st.s.adminCommand(
            {unshardCollection: dbName + '.' + shardedCollName3, toShard: shard1Rst.name}),
        ErrorCodes.IllegalOperation);
}

jsTest.log("Retry the addShard command");
assert.commandWorked(st.s.adminCommand({addShard: shard1Rst.getURL(), name: shard1Name}));

jsTest.log("Checking the cluster parameter");
// The addShard command should have set the cluster parameter to true again.
checkClusterParameter(st.configRS, true);
checkClusterParameter(st.rs0, true);
checkClusterParameter(shard1Rst, true);

jsTest.log("Check that data movement is now allowed");
assert.commandWorked(
    st.s.adminCommand({moveCollection: dbName + '.' + unshardedCollName, toShard: shard1Rst.name}));
// TODO (BACKPORT-20468) Enable these tests in multiversion once this has been backported to 8.0.
if (!isMultiversion) {
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: dbName + "." + shardedCollName1, find: {_id: 0}, to: shard1Rst.name}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: shard1Rst.name}));
    assert.commandWorked(st.s.adminCommand({
        reshardCollection: dbName + '.' + shardedCollName2,
        key: {_id: 1},
        forceRedistribution: true,
        shardDistribution: [{shard: shard1Rst.name, min: {_id: MinKey}, max: {_id: MaxKey}}]
    }));
    assert.commandWorked(st.s.adminCommand(
        {unshardCollection: dbName + '.' + shardedCollName3, toShard: shard1Rst.name}));
}

st.stop();
shard1Rst.stopSet();
