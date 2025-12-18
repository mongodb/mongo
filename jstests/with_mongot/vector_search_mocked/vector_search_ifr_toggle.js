/**
 * Tests that the featureFlagVectorSearchExtension IFR flag correctly toggles between extension
 * $vectorSearch (primary) and legacy $vectorSearch (fallback).
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    MongotMock,
    mongotResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    checkPlatformCompatibleWithExtensions,
    generateExtensionConfigWithOptions,
    deleteExtensionConfigs,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const dbName = jsTestName();
const collName = jsTestName();

// Generate extension config for the vector search extension.
const extensionName = generateExtensionConfigWithOptions("libvector_search_extension.so", "{}");

const mongotMock = new MongotMock();
mongotMock.start();
const conn = MongoRunner.runMongod({
    setParameter: {mongotHost: mongotMock.getConnection().host},
    loadExtensions: [extensionName],
});
const adminDb = conn.getDB("admin");
const testDB = conn.getDB(dbName);
assertCreateCollection(testDB, collName);
const collectionUUID = getUUIDFromListCollections(testDB, collName);

const coll = testDB.getCollection(collName);
coll.insert([
    {_id: 0, text: "apple"},
    {_id: 1, text: "banana"},
    {_id: 2, text: "cherry"},
]);

function testExtensionVectorSearch() {
    // Flag enabled; extension $vectorSearch expects an empty spec and acts as a no-op.
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagVectorSearchExtension: true}));
    const results = coll.aggregate([{$vectorSearch: {}}]).toArray();
    assert.eq(results.length, 3, "Extension should return all docs: " + tojson(results));
}

function testLegacyVectorSearch() {
    // Flag disabled; legacy $vectorSearch calls on mongotmock.
    assert.commandWorked(adminDb.runCommand({setParameter: 1, featureFlagVectorSearchExtension: false}));
    const queryVector = [1.0, 2.0, 3.0];
    const path = "x";
    const limit = 5;
    const mongotResponseBatch = [
        {_id: 0, $vectorSearchScore: 0.99},
        {_id: 1, $vectorSearchScore: 0.88},
    ];
    const expectedDocs = [
        {_id: 0, text: "apple"},
        {_id: 1, text: "banana"},
    ];
    mongotMock.setMockResponses(
        [
            {
                expectedCommand: mongotCommandForVectorSearchQuery({
                    queryVector,
                    path,
                    limit,
                    collName,
                    dbName,
                    collectionUUID,
                }),
                response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), dbName + "." + collName, 1),
            },
        ],
        NumberLong(123),
    );

    const results = coll.aggregate([{$vectorSearch: {queryVector, path, limit}}]).toArray();
    assert.eq(results, expectedDocs, "Legacy $vectorSearch should return mongot results: " + tojson(results));
}

try {
    testExtensionVectorSearch();
    testLegacyVectorSearch();
} finally {
    mongotMock.stop();
    MongoRunner.stopMongod(conn);
    deleteExtensionConfigs([extensionName]);
}
