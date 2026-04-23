/**
 * Tests that IFR flags are correctly propagated from router (mongos) to shards during aggregation
 * commands, and that the correct flag values are honored in different scenarios.
 *
 * @tags: [featureFlagExtensionsAPI, requires_profiling]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    generateExtensionConfigs,
    deleteExtensionConfigs,
    checkPlatformCompatibleWithExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const extensionNames = generateExtensionConfigs(["libvector_search_extension.so", "libsearch_extension.so"]);

const options = {
    loadExtensions: extensionNames,
};

const testData = [
    {_id: 0, vector: [1, 2, 3, 4], text: "poppi cans"},
    {_id: 1, vector: [0, 2, 4, 6], text: "homegrown tomatoes"},
    {_id: 2, vector: [3, 6, 9, 16], text: "crispy rice puffs"},
];
const vectorSearchPipeline = [{$vectorSearch: {}}];
const searchPipeline = [{$search: {}}];

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

function getShardPrimaries(shardingTest) {
    const primaries = [shardingTest.rs0.getPrimary()];
    if (shardingTest.rs1) {
        primaries.push(shardingTest.rs1.getPrimary());
    }
    return primaries;
}

function enableProfilingOnShards(shardingTest) {
    for (const primary of getShardPrimaries(shardingTest)) {
        assert.commandWorked(primary.getDB("test").setProfilingLevel(2));
    }
}

function assertIfrFlagOnShards(shardingTest, comment, flagName, expectedFlagValue) {
    let foundEntry = false;
    for (const primary of getShardPrimaries(shardingTest)) {
        const profileDB = primary.getDB("test");
        const entries = profileDB.system.profile
            .find({"command.comment": comment, "errName": {$ne: "StaleConfig"}})
            .toArray();

        for (const entry of entries) {
            foundEntry = true;
            // ifrFlags is a generic argument with forward_to_shards: true, so it appears at the
            // top level of the command. The fallback checks inside command.explain defensively in
            // case the shard's explain handler nests the aggregate command inside {explain: {...}}.
            const ifrFlags = entry.command.ifrFlags || (entry.command.explain && entry.command.explain.ifrFlags);
            assert(
                ifrFlags,
                "Expected ifrFlags in command on shard " +
                    primary.host +
                    ", comment: " +
                    comment +
                    ", command: " +
                    tojson(entry.command),
            );

            const flagEntry = ifrFlags.find((f) => f.name === flagName);
            assert(flagEntry, "Expected ifrFlags to contain " + flagName + ", got: " + tojson(ifrFlags));
            assert.eq(
                flagEntry.value,
                expectedFlagValue,
                "Expected " + flagName + " to be " + expectedFlagValue + ", got: " + flagEntry.value,
            );
        }
    }
    assert(foundEntry, "No profiler entries found with comment '" + comment + "' on any shard");
}

function setFlags(adminDb, shard0Admin, shard1Admin, flagName, routerFlag, shardFlag) {
    assert.commandWorked(adminDb.runCommand({setParameter: 1, [flagName]: routerFlag}));
    if (shard0Admin) {
        assert.commandWorked(shard0Admin.runCommand({setParameter: 1, [flagName]: shardFlag}));
    }
    if (shard1Admin) {
        assert.commandWorked(shard1Admin.runCommand({setParameter: 1, [flagName]: shardFlag}));
    }
}

function runIFRFlagPropagationTests(conn, shardingTest, flagName, pipeline) {
    const {adminDb, db, coll} = setupTestCollection(conn, shardingTest);
    const {shard0Admin, shard1Admin} = getShardAdmins(shardingTest);
    enableProfilingOnShards(shardingTest);

    // Test 1: Router flag=true propagates to shards (shards commit to true)
    setFlags(adminDb, shard0Admin, shard1Admin, flagName, /* routerFlag */ true, /* shardFlag */ false);
    const comment1 = "ifr_propagation_test_1_" + UUID().hex();
    assertArrayEq({
        actual: coll.aggregate(pipeline, {comment: comment1}).toArray(),
        expected: testData,
    });
    assertIfrFlagOnShards(shardingTest, comment1, flagName, /* expectedFlagValue */ true);

    // Test 2: Direct shard request uses shard's flag value
    if (shard0Admin) {
        assert.commandWorked(shard0Admin.runCommand({setParameter: 1, [flagName]: false}));
        const shard0Coll = shardingTest.rs0.getPrimary().getDB("test")[coll.getName()];
        assert.throwsWithCode(() => shard0Coll.aggregate(pipeline).toArray(), ErrorCodes.SearchNotEnabled);
        assert.commandWorked(shard0Admin.runCommand({setParameter: 1, [flagName]: true}));
        const shard0Result = shard0Coll.aggregate(pipeline).toArray();
        if (shard0Result.length === 0 && shard1Admin) {
            assert.commandWorked(shard1Admin.runCommand({setParameter: 1, [flagName]: true}));
            const shard1Coll = shardingTest.rs1.getPrimary().getDB("test")[coll.getName()];
            const shard1Result = shard1Coll.aggregate(pipeline).toArray();
            assert(shard1Result.length > 0);
        } else {
            assert(shard0Result.length > 0);
        }
    }

    // Test 3: Router flag=false propagates to shards (shards commit to false)
    setFlags(adminDb, shard0Admin, shard1Admin, flagName, /* routerFlag */ false, /* shardFlag */ true);
    assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), ErrorCodes.SearchNotEnabled);

    // Test 3b: Verify router flag=false is serialized to shards via a non-extension-stage pipeline
    const comment3b = "ifr_propagation_test_3b_" + UUID().hex();
    coll.aggregate([{$match: {}}], {comment: comment3b}).toArray();
    assertIfrFlagOnShards(shardingTest, comment3b, flagName, /* expectedFlagValue */ false);

    // Test 4: Explain propagates router flag=true to shards
    setFlags(adminDb, shard0Admin, shard1Admin, flagName, /* routerFlag */ true, /* shardFlag */ false);
    const comment4 = "ifr_propagation_test_4_" + UUID().hex();
    const explainResult = coll.explain().aggregate(pipeline, {comment: comment4});
    assert.commandWorked(explainResult);
    assertIfrFlagOnShards(shardingTest, comment4, flagName, /* expectedFlagValue */ true);

    // Test 5: Explain propagates router flag=false to shards
    setFlags(adminDb, shard0Admin, shard1Admin, flagName, /* routerFlag */ false, /* shardFlag */ true);
    assert.throwsWithCode(() => coll.explain().aggregate(pipeline), ErrorCodes.SearchNotEnabled);

    // Test 5b: Verify router flag=false is serialized to shards via a non-extension-stage explain
    const comment5b = "ifr_propagation_test_5b_" + UUID().hex();
    assert.commandWorked(coll.explain().aggregate([{$match: {}}], {comment: comment5b}));
    assertIfrFlagOnShards(shardingTest, comment5b, flagName, /* expectedFlagValue */ false);

    // Setup $unionWith collection
    const otherCollName = jsTestName() + "_other";
    const otherColl = db[otherCollName];
    otherColl.drop();
    assert.commandWorked(otherColl.insertMany(testData));
    const dbName = db.getName();
    assert.commandWorked(
        adminDb.runCommand({
            shardCollection: otherColl.getFullName(),
            key: {_id: 1},
        }),
    );
    const unionPipeline = [{$unionWith: {coll: otherCollName, pipeline: pipeline}}];

    // Tests 6-9 only verify that the router correctly enforces the IFR flag during parsing; there
    // is no verification of the flag on the shard. This is because a separate aggregate command is
    // sent to the $unionWith sub-pipeline, meaning that we cannot filter the profiler entries based
    // on the comment field on the shards.

    // Test 6: $unionWith propagates router flag=false to shards
    setFlags(adminDb, shard0Admin, shard1Admin, flagName, /* routerFlag */ true, /* shardFlag */ false);
    assertArrayEq({
        actual: coll.aggregate(unionPipeline).toArray(),
        expected: testData.concat(testData),
    });

    // Test 7: $unionWith propagates router flag=false to shards
    setFlags(adminDb, shard0Admin, shard1Admin, flagName, /* routerFlag */ false, /* shardFlag */ true);
    assert.throwsWithCode(() => coll.aggregate(unionPipeline).toArray(), ErrorCodes.SearchNotEnabled);

    // Test 8: $unionWith explain propagates router flag=true to shards
    setFlags(adminDb, shard0Admin, shard1Admin, flagName, /* routerFlag */ true, /* shardFlag */ false);
    const unionExplainResult = coll.explain().aggregate(unionPipeline);
    assert.commandWorked(unionExplainResult);

    // Test 9: $unionWith explain propagates router flag=false to shards
    setFlags(adminDb, shard0Admin, shard1Admin, flagName, /* routerFlag */ false, /* shardFlag */ true);
    assert.throwsWithCode(() => coll.explain().aggregate(unionPipeline), ErrorCodes.SearchNotEnabled);

    // Disable profiling on shards after tests complete.
    for (const primary of getShardPrimaries(shardingTest)) {
        assert.commandWorked(primary.getDB("test").setProfilingLevel(0));
    }
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
    runIFRFlagPropagationTests(
        multiShardTest.s,
        multiShardTest,
        "featureFlagVectorSearchExtension",
        vectorSearchPipeline,
    );
    runIFRFlagPropagationTests(multiShardTest.s, multiShardTest, "featureFlagSearchExtension", searchPipeline);
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
    runIFRFlagPropagationTests(
        singleShardTest.s,
        singleShardTest,
        "featureFlagVectorSearchExtension",
        vectorSearchPipeline,
    );
    runIFRFlagPropagationTests(singleShardTest.s, singleShardTest, "featureFlagSearchExtension", searchPipeline);
    singleShardTest.stop();
} finally {
    deleteExtensionConfigs(extensionNames);
}
