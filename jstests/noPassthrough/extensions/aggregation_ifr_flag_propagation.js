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
const testData = [
    {_id: 0, vector: [1, 2, 3, 4], text: "poppi cans"},
    {_id: 1, vector: [0, 2, 4, 6], text: "homegrown tomatoes"},
    {_id: 2, vector: [3, 6, 9, 16], text: "crispy rice puffs"},
];

function setupTestCollection(conn, shardingTest) {
    const adminDb = conn.getDB("admin");
    const db = conn.getDB("test");
    const coll = db[jsTestName()];
    coll.drop();
    assert.commandWorked(coll.insertMany(testData));

    const dbName = db.getName();
    assert.commandWorked(adminDb.runCommand({enableSharding: dbName}));
    assert.commandWorked(
        adminDb.runCommand({
            shardCollection: coll.getFullName(),
            key: {_id: 1},
        }),
    );

    return {adminDb, db, coll};
}

function getShardAdmins(shardingTest) {
    const shard0Admin = shardingTest.rs0.getPrimary().getDB("admin");
    const shard1Admin = shardingTest.rs1 ? shardingTest.rs1.getPrimary().getDB("admin") : null;
    return {shard0Admin, shard1Admin};
}

function setFlags(adminDb, shard0Admin, shard1Admin, routerFlag, shardFlag) {
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagVectorSearchExtension: routerFlag}));
    if (shard0Admin) {
        assert.commandWorked(shard0Admin.runCommand({setParameter: 1, featureFlagVectorSearchExtension: shardFlag}));
    }
    if (shard1Admin) {
        assert.commandWorked(shard1Admin.runCommand({setParameter: 1, featureFlagVectorSearchExtension: shardFlag}));
    }
}

function runIFRFlagPropagationTests(conn, shardingTest) {
    const {adminDb, db, coll} = setupTestCollection(conn, shardingTest);
    const {shard0Admin, shard1Admin} = getShardAdmins(shardingTest);

    // Test 1: Router flag=true propagates to shards (shards commit to true)
    setFlags(adminDb, shard0Admin, shard1Admin, /* routerFlag */ true, /* shardFlag */ false);
    assertArrayEq({
        actual: coll.aggregate(pipeline).toArray(),
        expected: testData,
    });

    // Test 2: Direct shard request uses shard's flag value
    if (shard0Admin) {
        assert.commandWorked(shard0Admin.runCommand({setParameter: 1, featureFlagVectorSearchExtension: false}));
        const shard0Coll = shardingTest.rs0.getPrimary().getDB("test")[coll.getName()];
        assert.throwsWithCode(() => shard0Coll.aggregate(pipeline).toArray(), ErrorCodes.SearchNotEnabled);
        assert.commandWorked(shard0Admin.runCommand({setParameter: 1, featureFlagVectorSearchExtension: true}));
        const shard0Result = shard0Coll.aggregate(pipeline).toArray();
        if (shard0Result.length === 0 && shard1Admin) {
            // If shard0 has no data, check shard1 instead
            assert.commandWorked(shard1Admin.runCommand({setParameter: 1, featureFlagVectorSearchExtension: true}));
            const shard1Coll = shardingTest.rs1.getPrimary().getDB("test")[coll.getName()];
            const shard1Result = shard1Coll.aggregate(pipeline).toArray();
            assert(shard1Result.length > 0);
        } else {
            assert(shard0Result.length > 0);
        }
    }

    // Test 3: Router flag=false propagates to shards (shards commit to false)
    setFlags(adminDb, shard0Admin, shard1Admin, /* routerFlag */ false, /* shardFlag */ true);
    assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), ErrorCodes.SearchNotEnabled);

    // Test 4: Explain propagates router flag=true to shards
    setFlags(adminDb, shard0Admin, shard1Admin, /* routerFlag */ true, /* shardFlag */ false);
    const explainResult = coll.explain().aggregate(pipeline);
    assert.commandWorked(explainResult);

    // Test 5: Explain propagates router flag=false to shards
    setFlags(adminDb, shard0Admin, shard1Admin, /* routerFlag */ false, /* shardFlag */ true);
    assert.throwsWithCode(() => coll.explain().aggregate(pipeline), ErrorCodes.SearchNotEnabled);

    // Setup $unionWith collection
    const otherCollName = jsTestName() + "_other";
    const otherColl = db[otherCollName];
    otherColl.drop();
    assert.commandWorked(otherColl.insertMany(testData));
    const dbName = db.getName();
    assert.commandWorked(adminDb.runCommand({enableSharding: dbName}));
    assert.commandWorked(
        adminDb.runCommand({
            shardCollection: otherColl.getFullName(),
            key: {_id: 1},
        }),
    );
    const unionPipeline = [{$unionWith: {coll: otherCollName, pipeline: pipeline}}];

    // Test 6: $unionWith propagates router flag=true to shards
    setFlags(adminDb, shard0Admin, shard1Admin, /* routerFlag */ true, /* shardFlag */ false);
    assertArrayEq({
        actual: coll.aggregate(unionPipeline).toArray(),
        expected: testData.concat(testData),
    });

    // Test 7: $unionWith propagates router flag=false to shards
    setFlags(adminDb, shard0Admin, shard1Admin, /* routerFlag */ false, /* shardFlag */ true);
    assert.throwsWithCode(() => coll.aggregate(unionPipeline).toArray(), ErrorCodes.SearchNotEnabled);

    // Test 8: $unionWith explain propagates router flag=true to shards
    setFlags(adminDb, shard0Admin, shard1Admin, /* routerFlag */ true, /* shardFlag */ false);
    const unionExplainResult = coll.explain().aggregate(unionPipeline);
    assert.commandWorked(unionExplainResult);

    // Test 9: $unionWith explain propagates router flag=false to shards
    setFlags(adminDb, shard0Admin, shard1Admin, /* routerFlag */ false, /* shardFlag */ true);
    assert.throwsWithCode(() => coll.explain().aggregate(unionPipeline), ErrorCodes.SearchNotEnabled);
}

try {
    const multiShardTest = new ShardingTest({
        shards: 2,
        rs: {nodes: 2},
        mongos: 1,
        config: 1,
        mongosOptions: options,
        configOptions: options,
        rsOptions: options,
    });
    runIFRFlagPropagationTests(multiShardTest.s, multiShardTest);
    multiShardTest.stop();

    const singleShardTest = new ShardingTest({
        shards: 1,
        rs: {nodes: 2},
        mongos: 1,
        config: 1,
        mongosOptions: options,
        configOptions: options,
        rsOptions: options,
    });
    runIFRFlagPropagationTests(singleShardTest.s, singleShardTest);
    singleShardTest.stop();
} finally {
    deleteExtensionConfigs(extensionNames);
}
