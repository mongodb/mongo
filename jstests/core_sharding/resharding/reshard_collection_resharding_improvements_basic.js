/**
 * Tests for basic functionality of features implemented in the resharding improvements project.
 * More specifically it tests shardDistribution, forceRedistribution and new index creation for a
 * shard key (if one does not exist).
 *
 * @tags: [
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements,
 *  # Stepdown test coverage is already provided by the resharding FSM suites.
 *  does_not_support_stepdowns,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReshardCollectionCmdTest} from "jstests/sharding/libs/reshard_collection_util.js";
import {getShardNames} from "jstests/sharding/libs/sharding_util.js";

// This test requires at least two shards.
const shardNames = getShardNames(db);
if (shardNames.length < 2) {
    jsTestLog("This test requires at least two shards.");
    quit();
}

const collName = jsTestName();
const dbName = db.getName();
const ns = dbName + '.' + collName;
const mongos = db.getMongo();
const kNumInitialDocs = 500;
const reshardCmdTest = new ReshardCollectionCmdTest({
    mongos,
    dbName: dbName,
    collName,
    numInitialDocs: kNumInitialDocs,
    skipDirectShardChecks: true
});

const testShardDistribution = (mongos) => {
    if (!FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements")) {
        jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled");
        return;
    }

    /**
     * Failure cases.
     */
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

    jsTest.log("reshardCollection cmd should fail when shardDistribution is missing min or max.");
    assert.commandFailedWithCode(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: shardNames[0], min: {newKey: MinKey}},
            {shard: shardNames[1], max: {newKey: MaxKey}}
        ]
    }),
                                 ErrorCodes.InvalidOptions);

    jsTest.log(
        "reshardCollection cmd should fail when shardDistribution is not specified using the shard key.");
    assert.commandFailedWithCode(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: shardNames[0], min: {oldKey: MinKey}, max: {oldKey: 0}},
            {shard: shardNames[1], min: {oldKey: 0}, max: {oldKey: MaxKey}}
        ]
    }),
                                 ErrorCodes.InvalidOptions);

    jsTest.log(
        "reshardCollection cmd should fail when one shard specifies min/max and the other does not.");
    assert.commandFailedWithCode(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: shardNames[0]},
            {shard: shardNames[1], min: {newKey: MinKey}, max: {newKey: MaxKey}}
        ]
    }),
                                 ErrorCodes.InvalidOptions);

    jsTest.log(
        "reshardCollection cmd should fail when shardDistribution is not starting with globalMin.");
    assert.commandFailedWithCode(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: shardNames[0], min: {newKey: -1}, max: {newKey: 0}},
            {shard: shardNames[1], min: {newKey: 0}, max: {newKey: MaxKey}}
        ]
    }),
                                 ErrorCodes.InvalidOptions);

    jsTest.log("reshardCollection cmd should fail when shardDistribution is not continuous.");
    assert.commandFailedWithCode(mongos.adminCommand({
        reshardCollection: ns,
        key: {newKey: 1},
        shardDistribution: [
            {shard: shardNames[0], min: {newKey: MinKey}, max: {newKey: -1}},
            {shard: shardNames[1], min: {newKey: 0}, max: {newKey: MaxKey}}
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
    mongos.getDB(dbName)[collName].drop();

    /**
     * Success cases go below.
     */
    jsTest.log("reshardCollection cmd should succeed with shardDistribution parameter.");
    reshardCmdTest.assertReshardCollOk({
        reshardCollection: ns,
        key: {newKey: 1},
        numInitialChunks: 2,
        shardDistribution: [{shard: shardNames[0]}, {shard: shardNames[1]}]
    },
                                       2);
    reshardCmdTest.assertReshardCollOk(
        {
            reshardCollection: ns,
            key: {newKey: 1},
            shardDistribution: [
                {shard: shardNames[0], min: {newKey: MinKey}, max: {newKey: 0}},
                {shard: shardNames[1], min: {newKey: 0}, max: {newKey: MaxKey}}
            ]
        },
        2,
        [
            {recipientShardId: shardNames[0], min: {newKey: MinKey}, max: {newKey: 0}},
            {recipientShardId: shardNames[1], min: {newKey: 0}, max: {newKey: MaxKey}}
        ]);
};

const testForceRedistribution = (mongos) => {
    if (!FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements")) {
        jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled");
        return;
    }

    jsTest.log(
        "When forceRedistribution is not set to true, same-key resharding should have no effect");
    reshardCmdTest.assertReshardCollOk(
        {reshardCollection: ns, key: {oldKey: 1}, numInitialChunks: 2}, 1);
    reshardCmdTest.assertReshardCollOk(
        {reshardCollection: ns, key: {oldKey: 1}, numInitialChunks: 2, forceRedistribution: false},
        1);

    jsTest.log("When forceRedistribution is true, same-key resharding should take effect");
    reshardCmdTest.assertReshardCollOk(
        {reshardCollection: ns, key: {oldKey: 1}, numInitialChunks: 2, forceRedistribution: true},
        2);

    jsTest.log(
        "When only one shard is provided, we should reshard all data from MIN to MAX into it");
    reshardCmdTest.assertReshardCollOk(
        {
            reshardCollection: ns,
            key: {oldKey: 1},
            forceRedistribution: true,
            numInitialChunks: 1,
            shardDistribution:
                [{shard: shardNames[0], min: {oldKey: MinKey}, max: {oldKey: MaxKey}}]
        },
        1,
        [{recipientShardId: shardNames[0], min: {oldKey: MinKey}, max: {oldKey: MaxKey}}]);
    reshardCmdTest.assertReshardCollOk(
        {
            reshardCollection: ns,
            key: {oldKey: 1},
            forceRedistribution: true,
            numInitialChunks: 1,
            shardDistribution: [{shard: shardNames[0]}]
        },
        1,
        [{recipientShardId: shardNames[0], min: {oldKey: MinKey}, max: {oldKey: MaxKey}}]);

    // Create a sharded collection with 2 zones, then force same-key resharding without
    // specifying zones and the resharding should use existing 2 zones
    jsTest.log("When zones is not provided, use existing zones on the collection");
    const additionalSetup = function(test) {
        const st = test._st;
        const ns = test._ns;
        const zoneName1 = 'z1';
        const zoneName2 = 'z2';
        assert.commandWorked(db.adminCommand({addShardToZone: shardNames[0], zone: zoneName1}));
        assert.commandWorked(db.adminCommand({addShardToZone: shardNames[0], zone: zoneName2}));
        assert.commandWorked(db.adminCommand({addShardToZone: shardNames[1], zone: zoneName2}));
        assert.commandWorked(db.adminCommand({shardCollection: ns, key: {oldKey: 1}}));
        assert.commandWorked(db.adminCommand(
            {updateZoneKeyRange: ns, min: {oldKey: MinKey}, max: {oldKey: 0}, zone: zoneName1}));
        assert.commandWorked(db.adminCommand(
            {updateZoneKeyRange: ns, min: {oldKey: 0}, max: {oldKey: MaxKey}, zone: zoneName2}));
    };

    reshardCmdTest.assertReshardCollOk(
        {
            reshardCollection: ns,
            key: {oldKey: 1},
            forceRedistribution: true,
            shardDistribution: [
                {shard: shardNames[0], min: {oldKey: MinKey}, max: {oldKey: -1}},
                {shard: shardNames[0], min: {oldKey: -1}, max: {oldKey: 1}},
                {shard: shardNames[1], min: {oldKey: 1}, max: {oldKey: MaxKey}}
            ]
        },
        4,
        [
            {recipientShardId: shardNames[0], min: {oldKey: MinKey}, max: {oldKey: -1}},
            {recipientShardId: shardNames[0], min: {oldKey: -1}, max: {oldKey: 0}},
            {recipientShardId: shardNames[0], min: {oldKey: 0}, max: {oldKey: 1}},
            {recipientShardId: shardNames[1], min: {oldKey: 1}, max: {oldKey: MaxKey}}
        ],
        [
            {zone: "z1", min: {oldKey: MinKey}, max: {oldKey: 0}},
            {zone: "z2", min: {oldKey: 0}, max: {oldKey: MaxKey}}
        ],
        additionalSetup);
    jsTest.log("When empty zones is provided, should discard the existing zones.");
    reshardCmdTest.assertReshardCollOk(
        {
            reshardCollection: ns,
            key: {oldKey: 1},
            forceRedistribution: true,
            zones: [],
            shardDistribution: [
                {shard: shardNames[0], min: {oldKey: MinKey}, max: {oldKey: -1}},
                {shard: shardNames[0], min: {oldKey: -1}, max: {oldKey: 1}},
                {shard: shardNames[1], min: {oldKey: 1}, max: {oldKey: MaxKey}}
            ]
        },
        3,
        [
            {recipientShardId: shardNames[0], min: {oldKey: MinKey}, max: {oldKey: -1}},
            {recipientShardId: shardNames[0], min: {oldKey: -1}, max: {oldKey: 1}},
            {recipientShardId: shardNames[1], min: {oldKey: 1}, max: {oldKey: MaxKey}}
        ],
        [],
        additionalSetup);
};

const testReshardingWithIndex = (mongos) => {
    if (!FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements")) {
        jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled");
        return;
    }

    jsTest.log(
        "When there is no index on the new shard-key, we should create one during resharding.");

    const additionalSetup = function(test) {
        assert.commandWorked(
            test._mongos.getDB(test._dbName).getCollection(test._collName).createIndex({
                oldKey: 1
            }));
    };

    reshardCmdTest.assertReshardCollOk(
        {reshardCollection: ns, key: {newKey: 1}, numInitialChunks: 2},
        2,
        undefined,
        undefined,
        additionalSetup);
};

testShardDistribution(mongos);
testForceRedistribution(mongos);
testReshardingWithIndex(mongos);
