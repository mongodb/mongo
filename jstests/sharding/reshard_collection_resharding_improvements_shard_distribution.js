/**
 * Tests for more complex cases for shardDistribution parameter.
 *
 * @tags: [
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ReshardCollectionCmdTest} from "jstests/sharding/libs/reshard_collection_util.js";

const st = new ShardingTest({mongos: 1, shards: 5});
const kDbName = 'db';
const collName = 'foo';
const ns = kDbName + '.' + collName;
const mongos = st.s0;
const kNumInitialDocs = 500;
const reshardCmdTest =
    new ReshardCollectionCmdTest({st, dbName: kDbName, collName, numInitialDocs: kNumInitialDocs});

const criticalSectionTimeoutMS = 24 * 60 * 60 * 1000; /* 1 day */
const topology = DiscoverTopology.findConnectedNodes(mongos);
const coordinator = new Mongo(topology.configsvr.nodes[0]);
assert.commandWorked(coordinator.getDB("admin").adminCommand(
    {setParameter: 1, reshardingCriticalSectionTimeoutMillis: criticalSectionTimeoutMS}));

const testCompoundShardKey = (mongos) => {
    if (!FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements")) {
        jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled.");
        return;
    }

    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {num: 1, str: 1}}));
    let bulk = mongos.getDB(kDbName).getCollection(collName).initializeOrderedBulkOp();
    for (let x = 0; x < kNumInitialDocs; x++) {
        bulk.insert({str: x.toString(), num: x, obj: {str: x.toString(), num: x}});
    }
    assert.commandWorked(bulk.execute());

    jsTestLog("shardDistribution missing second shardKey should error.");
    const missSecondKeyCmd = {
        reshardCollection: ns,
        key: {num: 1, str: 1},
        forceRedistribution: true,
        shardDistribution: [
            {shard: st.shard0.shardName, min: {num: MinKey}, max: {num: 1}},
            {shard: st.shard1.shardName, min: {num: 1}, max: {num: MaxKey}}
        ]
    };
    assert.commandFailedWithCode(mongos.adminCommand(missSecondKeyCmd), ErrorCodes.InvalidOptions);

    jsTestLog("shardDistribution not continuous on second shardKey should error.");
    const notContinuousCmd = {
        reshardCollection: ns,
        key: {num: 1, str: 1},
        forceRedistribution: true,
        shardDistribution: [
            {shard: st.shard0.shardName, min: {num: MinKey, str: MinKey}, max: {num: 1, str: '1'}},
            {shard: st.shard1.shardName, min: {num: 2, str: '1'}, max: {num: MaxKey, str: MaxKey}}
        ]
    };
    assert.commandFailedWithCode(mongos.adminCommand(notContinuousCmd), ErrorCodes.InvalidOptions);

    jsTestLog("shardDistribution overlap on second shardKey should error.");
    const overlapCmd = {
        reshardCollection: ns,
        key: {num: 1, str: 1},
        forceRedistribution: true,
        shardDistribution: [
            {shard: st.shard0.shardName, min: {num: MinKey, str: MinKey}, max: {num: 1, str: '2'}},
            {shard: st.shard1.shardName, min: {num: 1, str: '1'}, max: {num: MaxKey, str: MaxKey}}
        ]
    };
    assert.commandFailedWithCode(mongos.adminCommand(overlapCmd), ErrorCodes.InvalidOptions);

    jsTestLog("shardDistribution second shardKey not start from min should error.");
    const missingMinCmd = {
        reshardCollection: ns,
        key: {num: 1, str: 1},
        forceRedistribution: true,
        shardDistribution: [
            {shard: st.shard0.shardName, min: {num: MinKey, str: '1'}, max: {num: 1, str: '2'}},
            {shard: st.shard1.shardName, min: {num: 1, str: '1'}, max: {num: MaxKey, str: MaxKey}}
        ]
    };
    assert.commandFailedWithCode(mongos.adminCommand(missingMinCmd), ErrorCodes.InvalidOptions);

    jsTestLog("shardDistribution second shardKey not end at max should error.");
    const missingMaxCmd = {
        reshardCollection: ns,
        key: {num: 1, str: 1},
        forceRedistribution: true,
        shardDistribution: [
            {shard: st.shard0.shardName, min: {num: MinKey, str: MinKey}, max: {num: 1, str: '2'}},
            {shard: st.shard1.shardName, min: {num: 1, str: '1'}, max: {num: MaxKey, str: '2'}}
        ]
    };
    assert.commandFailedWithCode(mongos.adminCommand(missingMaxCmd), ErrorCodes.InvalidOptions);

    jsTestLog("This shardDistribution is a valid so the reshardCollection should succeed.");
    const correctCmd = {
        reshardCollection: ns,
        key: {num: 1, str: 1},
        forceRedistribution: true,
        shardDistribution: [
            {shard: st.shard0.shardName, min: {num: MinKey, str: MinKey}, max: {num: 1, str: '1'}},
            {shard: st.shard1.shardName, min: {num: 1, str: '1'}, max: {num: MaxKey, str: MaxKey}}
        ]
    };
    assert.commandWorked(mongos.adminCommand(correctCmd));
    mongos.getDB(kDbName)[collName].drop();
};

const testMoreShardsAndZones = (mongos) => {
    if (!FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements")) {
        jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled");
        return;
    }

    /**
     * This test is to ensure we have correct behavior when we have more shards and zones
     * The setup is following:
     * - shard0 -> [z1, z2, z3]
     * - shard1 -> [z2]
     * - shard2 -> [z2, z3]
     * - shard3 -> [z3]
     * - shard4 -> [z3]
     *
     * The key ranges for zones are:
     * - z1 -> [Min, -1000), [1000, Max)
     * - z2 -> [-1000, -1)
     * - z3 -> [-1, 1000)
     */
    jsTestLog("ReshardCollection should succeed when shardDistribution and zones mix together");

    const additionalSetup = function(test) {
        const st = test._st;
        const ns = test._ns;
        const zoneName1 = 'z1';
        const zoneName2 = 'z2';
        const zoneName3 = 'z3';
        assert.commandWorked(
            st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: zoneName1}));
        assert.commandWorked(
            st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: zoneName2}));
        assert.commandWorked(
            st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: zoneName3}));
        assert.commandWorked(
            st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: zoneName2}));
        assert.commandWorked(
            st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: zoneName2}));
        assert.commandWorked(
            st.s.adminCommand({addShardToZone: st.shard2.shardName, zone: zoneName3}));
        assert.commandWorked(
            st.s.adminCommand({addShardToZone: st.shard3.shardName, zone: zoneName3}));
        assert.commandWorked(
            st.s.adminCommand({addShardToZone: st.shard4.shardName, zone: zoneName3}));
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {oldKey: 1}}));
        assert.commandWorked(st.s.adminCommand({
            updateZoneKeyRange: ns,
            min: {oldKey: MinKey},
            max: {oldKey: -1000},
            zone: zoneName1
        }));
        assert.commandWorked(st.s.adminCommand(
            {updateZoneKeyRange: ns, min: {oldKey: 1000}, max: {oldKey: MaxKey}, zone: zoneName1}));
        assert.commandWorked(st.s.adminCommand(
            {updateZoneKeyRange: ns, min: {oldKey: -1000}, max: {oldKey: -1}, zone: zoneName2}));
        assert.commandWorked(st.s.adminCommand(
            {updateZoneKeyRange: ns, min: {oldKey: -1}, max: {oldKey: 1000}, zone: zoneName3}));
    };

    reshardCmdTest.assertReshardCollOk(
        {
            reshardCollection: ns,
            key: {oldKey: 1},
            forceRedistribution: true,
            shardDistribution: [
                {shard: st.shard0.shardName, min: {oldKey: MinKey}, max: {oldKey: -100}},
                {shard: st.shard1.shardName, min: {oldKey: -100}, max: {oldKey: -10}},
                {shard: st.shard2.shardName, min: {oldKey: -10}, max: {oldKey: 0}},
                {shard: st.shard3.shardName, min: {oldKey: 0}, max: {oldKey: 10}},
                {shard: st.shard4.shardName, min: {oldKey: 10}, max: {oldKey: 100}},
                {shard: st.shard0.shardName, min: {oldKey: 100}, max: {oldKey: MaxKey}},
            ]
        },
        9,
        [
            {recipientShardId: st.shard0.shardName, min: {oldKey: MinKey}, max: {oldKey: -1000}},
            {recipientShardId: st.shard0.shardName, min: {oldKey: -1000}, max: {oldKey: -100}},
            {recipientShardId: st.shard1.shardName, min: {oldKey: -100}, max: {oldKey: -10}},
            {recipientShardId: st.shard2.shardName, min: {oldKey: -10}, max: {oldKey: -1}},
            {recipientShardId: st.shard2.shardName, min: {oldKey: -1}, max: {oldKey: 0}},
            {recipientShardId: st.shard3.shardName, min: {oldKey: 0}, max: {oldKey: 10}},
            {recipientShardId: st.shard4.shardName, min: {oldKey: 10}, max: {oldKey: 100}},
            {recipientShardId: st.shard0.shardName, min: {oldKey: 100}, max: {oldKey: 1000}},
            {recipientShardId: st.shard0.shardName, min: {oldKey: 1000}, max: {oldKey: MaxKey}},
        ],
        [
            {zone: "z1", min: {oldKey: MinKey}, max: {oldKey: -1000}},
            {zone: "z1", min: {oldKey: 1000}, max: {oldKey: MaxKey}},
            {zone: "z2", min: {oldKey: -1000}, max: {oldKey: -1}},
            {zone: "z3", min: {oldKey: -1}, max: {oldKey: 1000}}
        ],
        additionalSetup);
};

testCompoundShardKey(mongos);
testMoreShardsAndZones(mongos);
st.stop();
