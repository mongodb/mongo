/**
 * Tests that $vectorSearch with 'returnStoredSource' returns both full documents and metadata when
 * the cluster parameter is enabled.
 *
 * @tags: [ featureFlagStoredSourceVectorSearch, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    MongotMock,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = jsTestName();
const collName = jsTestName();
const collNS = dbName + "." + collName;

const queryVector = [1.0, 2.0, 3.0];
const path = "title";
const numCandidates = 10;
const limit = 3;
const index = "index";
const cursorId = NumberLong(200);
const responseOk = 1;
const returnStoredSource = true;

const pipeline = [
    {$vectorSearch: {queryVector, path, numCandidates, limit, index, returnStoredSource}},
    {$project: {_id: 1, score: {$meta: "vectorSearchScore"}, title: 1, arches: 1}}
];

function testStandaloneVectorSearchStoredSourceParameterOn(mongotMock, testDB, collectionUUID) {
    // Include a field not on mongod to make sure we are getting back mongot
    // documents.
    const mongotResponseBatch = [
        {$vectorSearchScore: 0.654, storedSource: {_id: 1, title: "mcdonald's", arches: true}},
        {$vectorSearchScore: 0.321, storedSource: {_id: 3, title: "burger king", arches: false}},
        {$vectorSearchScore: 0.123, storedSource: {_id: 2, title: "taco bell", arches: false}}
    ];

    const expectedDocs = [
        {_id: 1, score: 0.654, title: "mcdonald's", arches: true},
        {_id: 3, score: 0.321, title: "burger king", arches: false},
        {_id: 2, score: 0.123, title: "taco bell", arches: false}
    ];

    const expectedCommand = mongotCommandForVectorSearchQuery({
        queryVector,
        path,
        numCandidates,
        index,
        limit,
        returnStoredSource,
        collName,
        dbName,
        collectionUUID,
    });

    const history = [{
        expectedCommand,
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk)
    }];

    mongotMock.setMockResponses(history, cursorId);

    const results = testDB[collName].aggregate(pipeline).toArray();
    assertArrayEq({actual: results, expected: expectedDocs});
};

function testStandaloneVectorSearchStoredSourceParameterOff(mongotMock, testDB, collectionUUID) {
    // When the cluster parameter is off, $vectorSearch should always act as if returnStoredSource
    // is false, regardless of its actual value.
    const mongotResponseBatch = [
        {_id: 1, $vectorSearchScore: 0.654},
        {_id: 3, $vectorSearchScore: 0.321},
        {_id: 2, $vectorSearchScore: 0.123}
    ];

    const expectedDocs = [
        {_id: 1, score: 0.654, title: "mcdonald's"},
        {_id: 3, score: 0.321, title: "burger king"},
        {_id: 2, score: 0.123, title: "taco bell"}
    ];

    const expectedCommand = mongotCommandForVectorSearchQuery({
        queryVector,
        path,
        numCandidates,
        index,
        limit,
        returnStoredSource,
        collName,
        dbName,
        collectionUUID,
    });

    const history = [{
        expectedCommand,
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk),
    }];

    mongotMock.setMockResponses(history, cursorId);

    const results = testDB[collName].aggregate(pipeline).toArray();
    assertArrayEq({actual: results, expected: expectedDocs});
};

function testShardedVectorSearchStoredSourceParameterOn(
    stWithMock, testDB, collectionUUID, shard0Conn, shard1Conn) {
    // Set mock responses on each shard.
    const shard0Batch = [
        {$vectorSearchScore: 0.654, storedSource: {_id: 1, title: "mcdonald's", arches: true}},
    ];
    const history0 = [{
        expectedCommand: mongotCommandForVectorSearchQuery({
            queryVector,
            path,
            numCandidates,
            index,
            limit,
            returnStoredSource,
            collName,
            dbName,
            collectionUUID
        }),
        response: mongotResponseForBatch(shard0Batch, NumberLong(0), collNS, responseOk),
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId);

    const shard1Batch = [
        {$vectorSearchScore: 0.777, storedSource: {_id: 2, title: "taco bell", arches: false}},
        {$vectorSearchScore: 0.321, storedSource: {_id: 3, title: "burger king", arches: false}},
    ];
    const history1 = [{
        expectedCommand: mongotCommandForVectorSearchQuery({
            queryVector,
            path,
            numCandidates,
            index,
            limit,
            returnStoredSource,
            collName,
            dbName,
            collectionUUID
        }),
        response: mongotResponseForBatch(shard1Batch, NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId);

    // Run the aggregation and assert results.
    const results = testDB[collName].aggregate(pipeline).toArray();
    const expectedDocs = [
        {_id: 2, score: 0.777, title: "taco bell", arches: false},
        {_id: 1, score: 0.654, title: "mcdonald's", arches: true},
        {_id: 3, score: 0.321, title: "burger king", arches: false},
    ];
    assertArrayEq({actual: results, expected: expectedDocs});
};

function testShardedVectorSearchStoredSourceParameterOff(
    stWithMock, testDB, collectionUUID, shard0Conn, shard1Conn) {
    // When the cluster parameter is off, $vectorSearch should always act as if returnStoredSource
    // is false, regardless of its actual value.
    const shard0Batch = [
        {_id: 1, $vectorSearchScore: 0.654},
    ];
    const history0 = [{
        expectedCommand: mongotCommandForVectorSearchQuery({
            queryVector,
            path,
            numCandidates,
            index,
            limit,
            returnStoredSource,
            collName,
            dbName,
            collectionUUID
        }),
        response: mongotResponseForBatch(shard0Batch, NumberLong(0), collNS, responseOk),
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId);

    const shard1Batch = [{_id: 2, $vectorSearchScore: 0.777}, {_id: 3, $vectorSearchScore: 0.321}];
    const history1 = [{
        expectedCommand: mongotCommandForVectorSearchQuery({
            queryVector,
            path,
            numCandidates,
            index,
            limit,
            returnStoredSource,
            collName,
            dbName,
            collectionUUID
        }),
        response: mongotResponseForBatch(shard1Batch, NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId);

    // Run the aggregation and assert results.
    const results = testDB[collName].aggregate(pipeline).toArray();
    const expectedDocs = [
        {_id: 2, score: 0.777, title: "taco bell"},
        {_id: 1, score: 0.654, title: "mcdonald's"},
        {_id: 3, score: 0.321, title: "burger king"},
    ];
    assertArrayEq({actual: results, expected: expectedDocs});
};

(function testStandaloneVectorSearchStoredSource() {
    const mongotMock = new MongotMock();
    mongotMock.start();
    const mockConn = mongotMock.getConnection();

    // Start mongod pointing at mock mongot.
    const conn = MongoRunner.runMongod({setParameter: {mongotHost: mockConn.host}});

    const testDB = conn.getDB(dbName);
    assertCreateCollection(testDB, collName);
    const collectionUUID = getUUIDFromListCollections(testDB, collName);

    // Populate collection.
    const coll = testDB.getCollection(collName);
    coll.insert([
        {_id: 1, title: "mcdonald's"},
        {_id: 2, title: "taco bell"},
        {_id: 3, title: "burger king"}
    ]);

    // Enable cluster parameter.
    assert.commandWorked(testDB.adminCommand(
        {setClusterParameter: {internalVectorSearchStoredSource: {enabled: true}}}));
    testStandaloneVectorSearchStoredSourceParameterOn(mongotMock, testDB, collectionUUID);

    // Disable the cluster parameter and run tests again.
    assert.commandWorked(testDB.adminCommand(
        {setClusterParameter: {internalVectorSearchStoredSource: {enabled: false}}}));
    testStandaloneVectorSearchStoredSourceParameterOff(mongotMock, testDB, collectionUUID);

    // Re-enable the cluster parameter and run tests again.
    assert.commandWorked(testDB.adminCommand(
        {setClusterParameter: {internalVectorSearchStoredSource: {enabled: true}}}));
    testStandaloneVectorSearchStoredSourceParameterOn(mongotMock, testDB, collectionUUID);

    MongoRunner.stopMongod(conn);
    mongotMock.stop();
})();

(function testShardedVectorSearchStoredSource() {
    const stWithMock = new ShardingTestWithMongotMock({
        name: "sharded_vector_search",
        shards: {rs0: {nodes: 1}, rs1: {nodes: 1}},
        mongos: 1,
    });
    stWithMock.start();
    const st = stWithMock.st;

    const mongos = st.s;
    const testDB = mongos.getDB(dbName);
    const coll = testDB.getCollection(collName);

    // Enable sharding on the database and collection.
    assert.commandWorked(
        testDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

    // Populate collection.
    assert.commandWorked(coll.insert([
        {_id: 1, title: "mcdonald's", shardKey: 0},
        {_id: 2, title: "taco bell", shardKey: 100},
        {_id: 3, title: "burger king", shardKey: 100}
    ]));

    // Shard on shardKey.
    assert.commandWorked(coll.createIndex({shardKey: 1}));
    st.shardColl(coll, {shardKey: 1}, {shardKey: 100}, {shardKey: 100});

    const shard0Conn = st.rs0.getPrimary();
    const shard1Conn = st.rs1.getPrimary();

    const collectionUUID = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);

    // Enable cluster parameter.
    assert.commandWorked(testDB.adminCommand(
        {setClusterParameter: {internalVectorSearchStoredSource: {enabled: true}}}));
    testShardedVectorSearchStoredSourceParameterOn(
        stWithMock, testDB, collectionUUID, shard0Conn, shard1Conn);

    // Disable the cluster parameter and run tests again.
    assert.commandWorked(testDB.adminCommand(
        {setClusterParameter: {internalVectorSearchStoredSource: {enabled: false}}}));
    testShardedVectorSearchStoredSourceParameterOff(
        stWithMock, testDB, collectionUUID, shard0Conn, shard1Conn);

    // Re-enable the cluster parameter and run tests again.
    assert.commandWorked(testDB.adminCommand(
        {setClusterParameter: {internalVectorSearchStoredSource: {enabled: true}}}));
    testShardedVectorSearchStoredSourceParameterOn(
        stWithMock, testDB, collectionUUID, shard0Conn, shard1Conn);

    stWithMock.stop();
})();
