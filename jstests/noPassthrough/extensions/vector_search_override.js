/**
 * Tests that the extension $vectorSearch stage has overridden the server implementation
 * when loaded.
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

function runVectorSearchOverrideTest(conn, shardingTest = null) {
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

    // Test one $vectorSearch stage passes documents through unchanged.
    {
        const pipeline = [{$vectorSearch: {}}];
        const result = coll.aggregate(pipeline).toArray();

        assertArrayEq({actual: result, expected: testData});
    }

    // Test $vectorSearch stage at different positions in complex pipeline.
    {
        const pipeline = [
            {$vectorSearch: {}},
            {$match: {_id: {$in: [0, 1]}}},
            {$vectorSearch: {}},
            {$project: {y: "$text", _id: 0}},
            {$vectorSearch: {a: 0}},
            {$sort: {y: -1}},
        ];
        const result = coll.aggregate(pipeline).toArray();

        assertArrayEq({actual: result, expected: [{y: "homegrown tomatoes"}, {y: "poppi cans"}]});
    }
}

try {
    // Test $vectorSearch override on a standalone mongod.
    const mongodConn = MongoRunner.runMongod(options);
    runVectorSearchOverrideTest(mongodConn);
    MongoRunner.stopMongod(mongodConn);

    // Test $vectorSearch override in a sharded cluster.
    const shardingTest = new ShardingTest({
        shards: 2,
        rs: {nodes: 2},
        mongos: 1,
        config: 1,
        mongosOptions: options,
        configOptions: options,
        rsOptions: options,
    });
    runVectorSearchOverrideTest(shardingTest.s, shardingTest);
    shardingTest.stop();
} finally {
    deleteExtensionConfigs(extensionNames);
}
