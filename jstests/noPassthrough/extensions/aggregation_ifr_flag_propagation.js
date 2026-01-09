/**
 * Tests that IFR flags are correctly propagated from router (mongos) to shards during aggregation
 * commands, and that the correct flag values are honored in different scenarios.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    generateExtensionConfigs,
    deleteExtensionConfigs,
    checkPlatformCompatibleWithExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const extensionNames = generateExtensionConfigs("libvector_search_extension.so");

const options = {
    loadExtensions: extensionNames[0],
};

const pipeline = [{$vectorSearch: {}}];

function runIFRFlagPropagationTests(conn, shardingTest = null) {
    const adminDb = conn.getDB("admin");
    const db = conn.getDB("test");
    const coll = db[jsTestName()];
    coll.drop();
    const testData = [
        {_id: 0, vector: [1, 2, 3, 4], text: "poppi cans"},
        {_id: 1, vector: [0, 2, 4, 6], text: "homegrown tomatoes"},
        {_id: 2, vector: [3, 6, 9, 16], text: "crispy rice puffs"},
    ];
    assert.commandWorked(coll.insertMany(testData));
    if (shardingTest) {
        shardingTest.shardColl(coll, {_id: 1});
    }

    // Test 1: Verify IFR flag value propagated from router to shard.
    // Router has flag=true, request from router - shards should commit to true.
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagVectorSearchExtension: true}));
    const shard0Admin = shardingTest.rs0.getPrimary().getDB("admin");
    const shard1Admin = shardingTest.rs1.getPrimary().getDB("admin");
    assert.commandWorked(shard0Admin.runCommand({setParameter: 1, featureFlagVectorSearchExtension: false}));
    assert.commandWorked(shard1Admin.runCommand({setParameter: 1, featureFlagVectorSearchExtension: false}));
    assertArrayEq({
        actual: coll.aggregate(pipeline).toArray(),
        expected: testData,
    });

    // Test 2: Direct request to shard - uses shard's flag value.
    // Shard has flag = false, so legacy implementation should be used.
    assert.commandWorked(shard0Admin.runCommand({setParameter: 1, featureFlagVectorSearchExtension: false}));
    const shard0Coll = shardingTest.rs0.getPrimary().getDB("test")[jsTestName()];
    assert.throwsWithCode(() => shard0Coll.aggregate(pipeline).toArray(), ErrorCodes.SearchNotEnabled);
    // Set shard flag to true.
    assert.commandWorked(shard0Admin.runCommand({setParameter: 1, featureFlagVectorSearchExtension: true}));
    assert(shard0Coll.aggregate(pipeline).toArray().length > 0);

    // Test 3: Router has flag=false, request from router - shards should commit to false.
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagVectorSearchExtension: false}));
    assert.commandWorked(shard0Admin.runCommand({setParameter: 1, featureFlagVectorSearchExtension: true}));
    assert.commandWorked(shard1Admin.runCommand({setParameter: 1, featureFlagVectorSearchExtension: true}));
    assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), ErrorCodes.SearchNotEnabled);
}

try {
    // Test IFR flag propagation in a sharded cluster.
    const shardingTest = new ShardingTest({
        shards: 2,
        rs: {nodes: 2},
        mongos: 1,
        config: 1,
        mongosOptions: options,
        configOptions: options,
        rsOptions: options,
    });
    runIFRFlagPropagationTests(shardingTest.s, shardingTest);
    shardingTest.stop();
} finally {
    deleteExtensionConfigs(extensionNames);
}
