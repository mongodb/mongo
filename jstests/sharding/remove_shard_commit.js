/**
 * Tests the removeShardCommit coordinator while it is in development.
 *
 * TODO (SERVER-97828): remove this test once the new coordinator is used in the removeShard
 * command.
 *
 * @tags: [
 *   featureFlagUseTopologyChangeCoordinators,
 *   does_not_support_stepdowns,
 *   assumes_balancer_off,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardTransitionUtil} from "jstests/libs/shard_transition_util.js";
import {getShardNames} from "jstests/libs/sharded_cluster_fixture_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {checkClusterParameter} from "jstests/sharding/libs/cluster_cardinality_parameter_util.js";

const dbName = 'test';
const collName = 'foo';
const ns = dbName + '.' + collName;

function getTopologyTime(st) {
    const shardDoc = st.s.getDB('config')
                         .getCollection('shards')
                         .find({})
                         .sort({'topologyTime': -1})
                         .limit(1)
                         .toArray();
    assert.eq(shardDoc.length, 1);
    return shardDoc[0].topologyTime;
}

function moveAllChunksOffShard(st, shardName, otherShard) {
    let configDB = st.s.getDB("config");
    let collections = configDB.getCollection("collections").find().toArray();
    collections.forEach((coll) => {
        let chunksOnShard =
            configDB.getCollection("chunks").find({shard: shardName, uuid: coll.uuid}).toArray();
        chunksOnShard.forEach((chunk) => {
            assert.commandWorked(st.s.adminCommand(
                {moveRange: coll._id, min: chunk.min, toShard: otherShard, waitForDelete: true}));
        });
    });
}

const st = new ShardingTest({shards: 3});
let isConfigShard = TestData.configShard;
let standardRemoveShardCommand =
    isConfigShard ? "transitionToDedicatedConfigServer" : "removeShard";

jsTest.log("Test that the commit coordinator will return success if the shard does not exist");
let res = assert.commandWorked(st.configRS.getPrimary().adminCommand(
    {_configsvrRemoveShardCommit: "fakeShardName", writeConcern: {w: "majority"}}));
assert.eq(res.state, "completed");

jsTest.log("Test that the commit coordinator will fail if the shard to be removed is not draining");
assert.commandFailedWithCode(
    st.configRS.getPrimary().adminCommand(
        {_configsvrRemoveShardCommit: st.shard0.shardName, writeConcern: {w: "majority"}}),
    ErrorCodes.ConflictingOperationInProgress);

// Setup a sharded collection with a chunk on the first shard (this is the config shard in an
// embedded config server case).
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(
    st.s.adminCommand({moveRange: ns, min: {x: MinKey}, toShard: st.shard0.shardName}));

// Start draining on the shard to prevent failing in the precondition checks.
res = assert.commandWorked(st.s.adminCommand({[standardRemoveShardCommand]: st.shard0.shardName}));
assert.eq(res.state, "started");

jsTest.log(
    "Test that the commit coordinator will fail if there are chunks present on the shard being removed after ddls complete.");
res = assert.commandWorked(st.configRS.getPrimary().adminCommand(
    {_configsvrRemoveShardCommit: st.shard0.shardName, writeConcern: {w: "majority"}}));
assert.eq(res.state, "ongoing");

// Move all chunks off shard so the commit can progress.
moveAllChunksOffShard(st, st.shard0.shardName, st.shard1.shardName);

if (isConfigShard) {
    jsTest.log(
        "Test that the commit coordinator will fail if there is any data in a collection present on the shard being removed");
    assert.commandWorked(st.rs0.getPrimary().getDB(dbName).getCollection(collName).insert({x: 1}));
    res = assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrRemoveShardCommit: st.shard0.shardName, writeConcern: {w: "majority"}}));
    assert.eq(res.state, "pendingDataCleanup");
    assert.commandWorked(
        st.configRS.getPrimary().getDB(dbName).getCollection(collName).deleteMany({}));
}

// Manually delete the draining flag so that we can move some chunks around.
assert.commandWorked(st.s.getDB("config").runCommand({
    update: "shards",
    updates: [{q: {_id: st.shard0.shardName}, u: {$unset: {"draining": ""}}, multi: false}]
}));
// Move a chunk to and from the shard but leave range deletions present on the first shard.
assert.commandWorked(st.s.adminCommand(
    {moveRange: ns, min: {x: MinKey}, toShard: st.shard0.shardName, waitForDelete: true}));
let rangeDeletionFailpoint = configureFailPoint(st.configRS.getPrimary(), "suspendRangeDeletion");
assert.commandWorked(
    st.s.adminCommand({moveRange: ns, min: {x: MinKey}, toShard: st.shard1.shardName}));

// Start draining again so that we pass the precondition checks.
res = assert.commandWorked(st.s.adminCommand({[standardRemoveShardCommand]: st.shard0.shardName}));
assert.eq(res.state, "started");

if (isConfigShard) {
    // Check that the commit coordinator returns that range deletions need cleaned up.
    jsTest.log(
        "Test that the commit coordinator will wait for range deletions to complete if in a config shard scenario.");
    let res = assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrRemoveShardCommit: "config", writeConcern: {w: "majority"}}));
    assert.eq(res.state, "pendingDataCleanup");

    // Let the range deletions complete.
    rangeDeletionFailpoint.off();
    ShardTransitionUtil.waitForRangeDeletions(st.configRS.getPrimary());
}

// If we are in the config shard scenario, the range deletions are now cleaned up. If we are not
// in the config shard scenario, we should ignore the range deletions.
jsTest.log(
    "Test that the coordinator will commit the shard removal, bump the topology time, and unblock DDLs.");
let initialTopologyTime = getTopologyTime(st);
let shardName = st.shard0.shardName;
res = assert.commandWorked(st.configRS.getPrimary().adminCommand(
    {_configsvrRemoveShardCommit: st.shard0.shardName, writeConcern: {w: "majority"}}));
assert.eq(res.state, "completed");

let finalTopologyTime = getTopologyTime(st);
assert(!getShardNames(st.s).includes(shardName));
assert.gt(timestampCmp(finalTopologyTime, initialTopologyTime), 0);

assert.commandWorked(st.s.adminCommand({shardCollection: "test.dummyColl", key: {x: 1}}));

checkClusterParameter(st.configRS, true);

jsTest.log("Remove another shard and check the cluster cardinality parameter");
res = assert.commandWorked(st.s.adminCommand({removeShard: st.shard2.shardName}));
assert.eq(res.state, "started");
res = assert.commandWorked(st.configRS.getPrimary().adminCommand(
    {_configsvrRemoveShardCommit: st.shard2.shardName, writeConcern: {w: "majority"}}));
assert.eq(res.state, "completed");

// If replica set endpoint is enabled, we should drop the cluster cardinality parameter after
// removing the second shard.
let expectedValue = TestData.testingReplicaSetEndpoint ? false : true;
checkClusterParameter(st.configRS, expectedValue);

rangeDeletionFailpoint.off();
st.stop();
