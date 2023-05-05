/**
 * Tests for basic functionality of the resharding improvements feature.
 *
 * @tags: [
 *  require_fcv_71,
 *  featureFlagReshardingImprovements
 * ]
 */

(function() {
'use strict';

load("jstests/libs/feature_flag_util.js");

const st = new ShardingTest({mongos: 1, shards: 2});
const kDbName = 'db';
const collName = 'foo';
const ns = kDbName + '.' + collName;
const mongos = st.s0;

const testShardDistribution = (mongos) => {
    if (!FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements")) {
        jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled");
        return;
    }

    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

    jsTest.log("reshardCollection cmd should succeed with shardDistribution parameter.");
    assert.commandWorked(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [{shard: "shard-1"}, {shard: "shard-2"}]
    }));
    assert.commandWorked(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: "shard-1", min: {newKey: MinKey}, max: {newKey: 0}},
            {shard: "shard-2", min: {newKey: 0}, max: {newKey: MaxKey}}
        ]
    }));

    jsTest.log("reshardCollection cmd should fail when shardDistribution is not valid.");
    // TODO(SERVER-76615): Add tests for invalid shardDistribution parameter.
};

testShardDistribution(mongos);
st.stop();
})();
