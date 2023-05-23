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
load("jstests/sharding/libs/reshard_collection_util.js");

const st = new ShardingTest({mongos: 1, shards: 2});
const kDbName = 'db';
const collName = 'foo';
const ns = kDbName + '.' + collName;
const mongos = st.s0;
const kNumInitialDocs = 500;
const reshardCmdTest =
    new ReshardCollectionCmdTest({st, dbName: kDbName, collName, numInitialDocs: kNumInitialDocs});

const testShardDistribution = (mongos) => {
    if (!FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements")) {
        jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled");
        return;
    }

    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

    jsTest.log("reshardCollection cmd should fail when shardDistribution has duplicate shardId.");
    assert.commandFailedWithCode(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: st.shard0.shardName, min: {newKey: MinKey}},
            {shard: st.shard0.shardName, max: {newKey: MaxKey}}
        ]
    }),
                                 ErrorCodes.InvalidOptions);

    jsTest.log("reshardCollection cmd should fail when shardDistribution is missing min or max.");
    assert.commandFailedWithCode(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: st.shard0.shardName, min: {newKey: MinKey}},
            {shard: st.shard1.shardName, max: {newKey: MaxKey}}
        ]
    }),
                                 ErrorCodes.InvalidOptions);

    jsTest.log(
        "reshardCollection cmd should fail when shardDistribution is not specified using the shard key.");
    assert.commandFailedWithCode(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: st.shard0.shardName, min: {oldKey: MinKey}, max: {oldKey: 0}},
            {shard: st.shard1.shardName, min: {oldKey: 0}, max: {oldKey: MaxKey}}
        ]
    }),
                                 ErrorCodes.InvalidOptions);

    jsTest.log(
        "reshardCollection cmd should fail when one shard specifies min/max and the other does not.");
    assert.commandFailedWithCode(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: st.shard0.shardName},
            {shard: st.shard1.shardName, min: {newKey: MinKey}, max: {newKey: MaxKey}}
        ]
    }),
                                 ErrorCodes.InvalidOptions);

    jsTest.log(
        "reshardCollection cmd should fail when shardDistribution is not starting with globalMin.");
    assert.commandFailedWithCode(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: st.shard0.shardName, min: {newKey: -1}, max: {newKey: 0}},
            {shard: st.shard1.shardName, min: {newKey: 0}, max: {newKey: MaxKey}}
        ]
    }),
                                 ErrorCodes.InvalidOptions);

    jsTest.log("reshardCollection cmd should fail when shardDistribution is not continuous.");
    assert.commandFailedWithCode(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: st.shard0.shardName, min: {newKey: MinKey}, max: {newKey: -1}},
            {shard: st.shard1.shardName, min: {newKey: 0}, max: {newKey: MaxKey}}
        ]
    }),
                                 ErrorCodes.InvalidOptions);

    jsTest.log(
        "reshardCollection cmd should fail when the shardId in shardDistribution is not recognized.");
    assert.commandFailedWithCode(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: "s1", min: {newKey: MinKey}, max: {newKey: 0}},
            {shard: "s2", min: {newKey: 0}, max: {newKey: MaxKey}}
        ]
    }),
                                 ErrorCodes.ShardNotFound);

    jsTest.log("reshardCollection cmd should succeed with shardDistribution parameter.");
    reshardCmdTest.assertReshardCollOk({
        reshardCollection: ns,
        key: {newKey: 1},
        numInitialChunks: 2,
        shardDistribution: [{shard: st.shard0.shardName}, {shard: st.shard1.shardName}]
    },
                                       2);
    reshardCmdTest.assertReshardCollOk(
        {
            reshardCollection: ns,
            key: {newKey: 1},
            shardDistribution: [
                {shard: st.shard0.shardName, min: {newKey: MinKey}, max: {newKey: 0}},
                {shard: st.shard1.shardName, min: {newKey: 0}, max: {newKey: MaxKey}}
            ]
        },
        2,
        [
            {recipientShardId: st.shard0.shardName, min: {newKey: MinKey}, max: {newKey: 0}},
            {recipientShardId: st.shard1.shardName, min: {newKey: 0}, max: {newKey: MaxKey}}
        ]);
};

testShardDistribution(mongos);
st.stop();
})();
