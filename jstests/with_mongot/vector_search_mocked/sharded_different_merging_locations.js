/**
 * Sharding tests that cover a variety of different possible distributed execution scenarios.
 * @tags: [
 *   requires_fcv_71,
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";
import {prepCollection} from "jstests/with_mongot/mongotmock/lib/utils.js";

const dbName = "test";
const collName = jsTestName();

let nodeOptions = {setParameter: {enableTestCommands: 1}};
const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_vector_search",
    shards: {
        rs0: {nodes: 2},
        rs1: {nodes: 2},
    },
    mongos: 1,
    other: {
        rsOptions: nodeOptions,
        mongosOptions: nodeOptions,
        shardOptions: nodeOptions,
    }
});
stWithMock.start();
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

function setupCollection() {
    const testColl = testDB.getCollection(collName);

    prepCollection(mongos, dbName, collName);

    // Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
    st.shardColl(testColl, {_id: 1}, {_id: 10}, {_id: 10 + 1});

    return testColl;
}
const vectorSearchQuery = {
    queryVector: [1.0, 2.0, 3.0],
    path: "x",
    numCandidates: 10,
    limit: 5
};
const cursorId = NumberLong(123);
let testColl = setupCollection(collName);
// View queries resolve to the base namespace, so always use this.
const collNS = testColl.getFullName();
const collUUID = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), testColl.getName());

function testMergeAtLocation(mergeType, localColl, isView, limit = Infinity) {
    const pipeline = [
        {$vectorSearch: vectorSearchQuery},
        {$_internalSplitPipeline: {"mergeType": mergeType}},
        {$project: {_id: 1}},
    ];
    // A view already has the search stage
    if (isView) {
        pipeline.shift();
    }
    if (limit != Infinity) {
        pipeline.push({$limit: limit});
    }
    const responseOk = 1;

    const mongot0ResponseBatch = [
        {_id: 3, $vectorSearchScore: 0.99},
        {_id: 2, $vectorSearchScore: 0.10},
        {_id: 4, $vectorSearchScore: 0.02},
        {_id: 1, $vectorSearchScore: 0.01},
    ];
    const mongot0Response =
        mongotResponseForBatch(mongot0ResponseBatch, NumberLong(0), collNS, responseOk);
    const history0 = [{
        expectedCommand: mongotCommandForVectorSearchQuery(
            {...vectorSearchQuery, collName: testColl.getName(), dbName, collectionUUID: collUUID}),
        response: mongot0Response
    }];

    const mongot1ResponseBatch = [
        {_id: 11, $vectorSearchScore: 1.0},
        {_id: 13, $vectorSearchScore: 0.30},
        {_id: 12, $vectorSearchScore: 0.29},
        {_id: 14, $vectorSearchScore: 0.28},
    ];
    const mongot1Response =
        mongotResponseForBatch(mongot1ResponseBatch, NumberLong(0), collNS, responseOk);
    const history1 = [{
        expectedCommand: mongotCommandForVectorSearchQuery(
            {...vectorSearchQuery, collName: testColl.getName(), dbName, collectionUUID: collUUID}),
        response: mongot1Response
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());
    const s1Mongot = stWithMock.getMockConnectedToHost(st.rs1.getPrimary());
    s0Mongot.setMockResponses(history0, cursorId);
    s1Mongot.setMockResponses(history1, cursorId);

    const expectedDocs = [
        {_id: 11},
        {_id: 3},
        {_id: 13},
        {_id: 12},
        {_id: 14},
        {_id: 2},
        {_id: 4},
        {_id: 1},
    ];

    assert.eq(localColl.aggregate(pipeline).toArray(),
              expectedDocs.slice(0, Math.min(vectorSearchQuery.limit, limit)));
}

const owningShardMerge = {
    "specificShard": st.shard0.shardName
};
testMergeAtLocation("mongos", testColl, false);
testMergeAtLocation("mongos", testColl, false, 3);
testMergeAtLocation("mongos", testColl, false, 5);
testMergeAtLocation("mongos", testColl, false, 10);
testMergeAtLocation("anyShard", testColl, false);
testMergeAtLocation("anyShard", testColl, false, 3);
testMergeAtLocation("anyShard", testColl, false, 5);
testMergeAtLocation("anyShard", testColl, false, 10);
testMergeAtLocation(owningShardMerge, testColl, false);
testMergeAtLocation(owningShardMerge, testColl, false, 3);
testMergeAtLocation(owningShardMerge, testColl, false, 5);
testMergeAtLocation(owningShardMerge, testColl, false, 10);
testMergeAtLocation("localOnly", testColl, false);
testMergeAtLocation("localOnly", testColl, false, 3);
testMergeAtLocation("localOnly", testColl, false, 5);
testMergeAtLocation("localOnly", testColl, false, 10);

// Repeat, but the collection is a view.
testDB.createView(
    collName + "viewColl", testColl.getName(), [{$vectorSearch: vectorSearchQuery}], {});
let viewColl = testDB.getCollection(collName + "viewColl");

testMergeAtLocation("mongos", viewColl, true);
testMergeAtLocation("mongos", viewColl, true, 3);
testMergeAtLocation("mongos", viewColl, true, 5);
testMergeAtLocation("mongos", viewColl, true, 10);
testMergeAtLocation("anyShard", viewColl, true);
testMergeAtLocation("anyShard", viewColl, true, 3);
testMergeAtLocation("anyShard", viewColl, true, 5);
testMergeAtLocation("anyShard", viewColl, true, 10);
testMergeAtLocation(owningShardMerge, viewColl, true);
testMergeAtLocation(owningShardMerge, viewColl, true, 3);
testMergeAtLocation(owningShardMerge, viewColl, true, 5);
testMergeAtLocation(owningShardMerge, viewColl, true, 10);
testMergeAtLocation("localOnly", viewColl, true);
testMergeAtLocation("localOnly", viewColl, true, 3);
testMergeAtLocation("localOnly", viewColl, true, 5);
testMergeAtLocation("localOnly", viewColl, true, 10);

stWithMock.stop();
