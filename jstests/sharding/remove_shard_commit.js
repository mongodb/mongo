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
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = 'test';
const collName = 'foo';
const ns = dbName + '.' + collName;

const st = new ShardingTest({shards: 2});
let isConfigShard = TestData.configShard;

// Setup a sharded collection with range deletions on the first shard (this is the config shard
// if we are in an embedded config server case).
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(
    st.s.adminCommand({moveRange: ns, min: {x: MinKey}, toShard: st.shard0.shardName}));
let rangeDeletionFailpoint = configureFailPoint(st.configRS.getPrimary(), "suspendRangeDeletion");
assert.commandWorked(
    st.s.adminCommand({moveRange: ns, min: {x: MinKey}, toShard: st.shard1.shardName}));

if (isConfigShard) {
    // Check that the commit coordinator returns that range deletions need cleaned up.
    let res = assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrRemoveShardCommit: "config", writeConcern: {w: "majority"}}));
    assert.eq(res.state, "pendingDataCleanup");

    // Let the range deletions complete.
    rangeDeletionFailpoint.off();
    ShardTransitionUtil.waitForRangeDeletions(st.configRS.getPrimary());
}

// If we are in the config shard scenario, the range deletions are now cleaned up. If we are not
// in the config shard scenario, we should ignore the range deletions.
assert.commandFailedWithCode(
    st.configRS.getPrimary().adminCommand(
        {_configsvrRemoveShardCommit: st.shard0.shardName, writeConcern: {w: "majority"}}),
    ErrorCodes.NotImplemented);

rangeDeletionFailpoint.off();
st.stop();
