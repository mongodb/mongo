/**
 * Sharding tests for the `$vectorSearch` aggregation pipeline stage.
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
const collName = "sharded_vector_search";

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_vector_search",
    shards: {
        rs0: {nodes: 2},
        rs1: {nodes: 2},
    },
    mongos: 1,
    other: {
        rsOptions: {setParameter: {enableTestCommands: 1}},
    }
});
stWithMock.start();
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const testColl = testDB.getCollection(collName);
const collNS = testColl.getFullName();

prepCollection(mongos, dbName, collName);

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
st.shardColl(testColl, {_id: 1}, {_id: 10}, {_id: 10 + 1});

const collectionUUID = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);

let vectorSearchQuery = {
    "index": "default",
    "path": "x",
    "numCandidates": 10,
    "limit": 100,
    "filter": {"$or": [{"color": {"$gt": "C"}}, {"color": {"$lt": "C"}}]},
    "queryVector": [2.0, 2.0]
};
let expectedMongotCommand =
    mongotCommandForVectorSearchQuery({...vectorSearchQuery, collName, dbName, collectionUUID});

const cursorId = NumberLong(123);
let pipeline = [{$vectorSearch: vectorSearchQuery}, {$project: {x: 1, y: 1}}];

function runTestOnPrimaries(testFn) {
    testDB.getMongo().setReadPref("primary");
    testFn(st.rs0.getPrimary(), st.rs1.getPrimary());
}

function runTestOnSecondaries(testFn) {
    testDB.getMongo().setReadPref("secondary");
    testFn(st.rs0.getSecondary(), st.rs1.getSecondary());
}

// Tests that $vectorSearch returns documents in descending $meta: vectorSearchScore.
function testBasicSortCase(shard0Conn, shard1Conn) {
    const responseOk = 1;

    const mongot0ResponseBatch = [
        {_id: 3, $vectorSearchScore: 0.99},
        {_id: 2, $vectorSearchScore: 0.10},
        {_id: 4, $vectorSearchScore: 0.01},
        {_id: 1, $vectorSearchScore: 0.0099},
    ];
    const history0 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotResponseForBatch(mongot0ResponseBatch, NumberLong(0), collNS, responseOk),
    }];

    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId);

    const mongot1ResponseBatch = [
        {_id: 11, $vectorSearchScore: 1.0},
        {_id: 13, $vectorSearchScore: 0.30},
        {_id: 12, $vectorSearchScore: 0.29},
        {_id: 14, $vectorSearchScore: 0.28},
    ];
    const history1 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotResponseForBatch(mongot1ResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId);

    const expectedDocs = [
        {_id: 11, x: "brown", y: "ipsum"},
        {_id: 3, x: "brown", y: "ipsum"},
        {_id: 13, x: "brown", y: "ipsum"},
        {_id: 12, x: "cow", y: "lorem ipsum"},
        {_id: 14, x: "cow", y: "lorem ipsum"},
        {_id: 2, x: "now", y: "lorem"},
        {_id: 4, x: "cow", y: "lorem ipsum"},
        {_id: 1, x: "ow"},
    ];

    assert.eq(testColl.aggregate(pipeline).toArray(), expectedDocs);
}
runTestOnPrimaries(testBasicSortCase);
runTestOnSecondaries(testBasicSortCase);

// Tests the case where there's an error from one mongot, which should get propagated to
// mongod, then to mongos.
function testErrorCase(shard0Conn, shard1Conn) {
    const responseOk = 1;

    const mongot0ResponseBatch = [
        {_id: 3, $vectorSearchScore: 1.00},
    ];
    const history0 = [
        {
            expectedCommand: expectedMongotCommand,
            response: mongotResponseForBatch(mongot0ResponseBatch, cursorId, collNS, responseOk),
        },
        {
            expectedCommand: {getMore: cursorId, collection: collName},
            response: {
                ok: 0,
                errmsg: "mongot error",
                code: ErrorCodes.InternalError,
                codeName: "InternalError"
            }
        }
    ];
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId);

    const mongot1ResponseBatch = [
        {_id: 11, $vectorSearchScore: 1.0},
        {_id: 13, $vectorSearchScore: 0.30},
        {_id: 12, $vectorSearchScore: 0.29},
        {_id: 14, $vectorSearchScore: 0.28},
    ];
    const history1 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotResponseForBatch(mongot1ResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId);

    const err = assert.throws(() => testColl.aggregate(pipeline).toArray());
    assert.commandFailedWithCode(err, ErrorCodes.InternalError);
}
runTestOnPrimaries(testErrorCase);
runTestOnSecondaries(testErrorCase);

// Tests the case where the mongot associated with one shard returns a small one batch data
// set, and the mongot associated with the other shard returns more data.
function testUnevenResultDistributionCase(shard0Conn, shard1Conn) {
    const responseOk = 1;

    const mongot0ResponseBatch = [
        {_id: 3, $vectorSearchScore: 0.99},
    ];
    const history0 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotResponseForBatch(mongot0ResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId);

    const history1 = [
        {
            expectedCommand: expectedMongotCommand,
            response: mongotResponseForBatch(
                [{_id: 11, $vectorSearchScore: 1.0}, {_id: 13, $vectorSearchScore: 0.30}],
                cursorId,
                collNS,
                responseOk),
        },

        {
            expectedCommand: {getMore: cursorId, collection: collName},
            response: mongotResponseForBatch(
                [{_id: 12, $vectorSearchScore: 0.29}, {_id: 14, $vectorSearchScore: 0.28}],
                NumberLong(0),
                collNS,
                responseOk)
        }
    ];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId);

    const expectedDocs = [
        {_id: 11, x: "brown", y: "ipsum"},
        {_id: 3, x: "brown", y: "ipsum"},
        {_id: 13, x: "brown", y: "ipsum"},
        {_id: 12, x: "cow", y: "lorem ipsum"},
        {_id: 14, x: "cow", y: "lorem ipsum"},
    ];

    assert.eq(testColl.aggregate(pipeline).toArray(), expectedDocs);
}
runTestOnPrimaries(testUnevenResultDistributionCase);
runTestOnSecondaries(testUnevenResultDistributionCase);

// Tests the case where a mongot does not actually return documents in the right order.
function testMisbehavingMongot(shard0Conn, shard1Conn) {
    const responseOk = 1;

    // mongot 0 returns results in the wrong order!
    const mongot0ResponseBatch = [
        {_id: 2, $vectorSearchScore: 0.10},
        {_id: 1, $vectorSearchScore: 0.0099},
        {_id: 3, $vectorSearchScore: 0.99},
        {_id: 4, $vectorSearchScore: 0.01},
    ];
    const history0 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotResponseForBatch(mongot0ResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId);

    const mongot1ResponseBatch = [
        {_id: 12, $vectorSearchScore: 0.29},
        {_id: 14, $vectorSearchScore: 0.28},
        {_id: 11, $vectorSearchScore: 1.0},
        {_id: 13, $vectorSearchScore: 0.30},
    ];
    const history1 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotResponseForBatch(mongot1ResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId);

    const expectedDocs = [
        {_id: 11, x: "brown", y: "ipsum"},
        {_id: 3, x: "brown", y: "ipsum"},
        {_id: 13, x: "brown", y: "ipsum"},
        {_id: 12, x: "cow", y: "lorem ipsum"},
        {_id: 14, x: "cow", y: "lorem ipsum"},
        {_id: 2, x: "now", y: "lorem"},
        {_id: 4, x: "cow", y: "lorem ipsum"},
        {_id: 1, x: "ow"},
    ];

    const res = testColl.aggregate(pipeline).toArray();

    // In this case, mongod returns incorrect results (and doesn't crash). Check that the _set_
    // of results returned is correct, ignoring ordering.
    assert.sameMembers(res, expectedDocs);
}
runTestOnPrimaries(testMisbehavingMongot);
runTestOnSecondaries(testMisbehavingMongot);

// Tests that correct results are returned when $vectorSearch is followed by a $sort on a
// different key.
function testSearchFollowedBySortOnDifferentKey(shard0Conn, shard1Conn) {
    const responseOk = 1;

    const mongot0ResponseBatch = [
        {_id: 3, $vectorSearchScore: 0.99},
        {_id: 2, $vectorSearchScore: 0.10},
        {_id: 4, $vectorSearchScore: 0.01},
        {_id: 1, $vectorSearchScore: 0.0099},
    ];
    const history0 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotResponseForBatch(mongot0ResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId);

    const mongot1ResponseBatch = [
        {_id: 11, $vectorSearchScore: 1.0},
        {_id: 13, $vectorSearchScore: 0.30},
        {_id: 12, $vectorSearchScore: 0.29},
        {_id: 14, $vectorSearchScore: 0.28},
    ];
    const history1 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotResponseForBatch(mongot1ResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId);

    const expectedDocs = [
        {_id: 1, x: "ow"},
        {_id: 2, x: "now", y: "lorem"},
        {_id: 3, x: "brown", y: "ipsum"},
        {_id: 4, x: "cow", y: "lorem ipsum"},
        {_id: 11, x: "brown", y: "ipsum"},
        {_id: 12, x: "cow", y: "lorem ipsum"},
        {_id: 13, x: "brown", y: "ipsum"},
        {_id: 14, x: "cow", y: "lorem ipsum"},
    ];

    assert.eq(testColl.aggregate(pipeline.concat([{$sort: {_id: 1}}])).toArray(), expectedDocs);
}
runTestOnPrimaries(testSearchFollowedBySortOnDifferentKey);
runTestOnSecondaries(testSearchFollowedBySortOnDifferentKey);

function testGetMoreOnShard(shard0Conn, shard1Conn) {
    const responseOk = {ok: 1};

    // Mock response from shard 0's mongot.
    const mongot0ResponseBatch = [
        {_id: 2, $vectorSearchScore: 1.0},
        {_id: 3, $vectorSearchScore: 0.99},
        {_id: 1, $vectorSearchScore: 0.29},
        {_id: 4, $vectorSearchScore: 0.01},
    ];
    const history0 = [
        {
            expectedCommand: expectedMongotCommand,
            response: mongotResponseForBatch(
                mongot0ResponseBatch.slice(0, 2), NumberLong(10), collNS, responseOk),
        },
        {
            expectedCommand: {getMore: NumberLong(10), collection: collName},
            response: {
                cursor: {
                    id: NumberLong(0),
                    ns: collName,
                    nextBatch: mongot0ResponseBatch.slice(2),
                },
                ok: 1,
            }
        },
    ];

    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, NumberLong(10));

    // Mock response from shard 1's mongot.
    const mongot1ResponseBatch = [
        {_id: 14, $vectorSearchScore: 0.28},
        {_id: 11, $vectorSearchScore: 0.10},
        {_id: 13, $vectorSearchScore: 0.02},
        {_id: 12, $vectorSearchScore: 0.0099},
    ];
    const history1 = [
        {
            expectedCommand: expectedMongotCommand,
            response: mongotResponseForBatch(
                mongot1ResponseBatch.slice(0, 1), NumberLong(30), collNS, responseOk),
        },
        {
            expectedCommand: {getMore: NumberLong(30), collection: collName},
            response: {
                cursor: {
                    id: NumberLong(0),
                    ns: collName,
                    nextBatch: mongot1ResponseBatch.slice(1),
                },
                ok: 1,
            }
        },
    ];

    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, NumberLong(30));

    const expectedDocs = [
        {_id: 2, x: "now"},
        {_id: 3, x: "brown"},
        {_id: 1, x: "ow"},
        {_id: 14, x: "cow"},
        {_id: 11, x: "brown"},
        {_id: 13, x: "brown"},
        {_id: 4, x: "cow"},
        {_id: 12, x: "cow"},
    ];

    assert.eq(expectedDocs,
              testColl
                  .aggregate([
                      {$vectorSearch: vectorSearchQuery},
                      {$project: {_id: 1, x: 1}},
                  ])
                  .toArray());
}
runTestOnPrimaries(testGetMoreOnShard);
runTestOnSecondaries(testGetMoreOnShard);

function testVectorSearchMultipleBatches(shard0Conn, shard1Conn) {
    const responseOk = {ok: 1};
    const vectorSearchQueryLimit =
        {queryVector: [1.0, 2.0, 3.0], path: "x", numCandidates: 10, limit: 5};

    const pipeline = [
        {$vectorSearch: vectorSearchQueryLimit},
        {$project: {_id: 1, score: {$meta: "vectorSearchScore"}, x: 1, y: 1}}
    ];

    const expectedMongotCommandLimit = mongotCommandForVectorSearchQuery(
        {...vectorSearchQueryLimit, collName, dbName, collectionUUID});

    const mongot0ResponseBatch = [
        {_id: 4, $vectorSearchScore: 1.0},
        {_id: 1, $vectorSearchScore: 0.9},
        {_id: 2, $vectorSearchScore: 0.8},
    ];

    const history0 = [{
        expectedCommand: expectedMongotCommandLimit,
        response: mongotResponseForBatch(mongot0ResponseBatch, NumberLong(0), collNS, responseOk),
    }];

    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId);

    const mongot1ResponseBatch = [
        {_id: 14, $vectorSearchScore: 0.99},
        {_id: 11, $vectorSearchScore: 0.2},
        {_id: 13, $vectorSearchScore: 0.1},
    ];

    const history1 = [{
        expectedCommand: expectedMongotCommandLimit,
        response: mongotResponseForBatch(mongot1ResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId);

    const expectedDocs = [
        {_id: 4, x: "cow", y: "lorem ipsum", score: 1.0},
        {_id: 14, x: "cow", y: "lorem ipsum", score: 0.99},
        {_id: 1, x: "ow", score: 0.9},
        {_id: 2, x: "now", y: "lorem", score: 0.8},
        {_id: 11, x: "brown", y: "ipsum", score: 0.2},
    ];

    assert.eq(testDB[collName].aggregate(pipeline).toArray(), expectedDocs);
}
runTestOnPrimaries(testVectorSearchMultipleBatches);
runTestOnSecondaries(testVectorSearchMultipleBatches);

// Tests that $vectorSearch returns the correct number of documents.
function testBasicLimitCase(shard0Conn, shard1Conn) {
    const vectorSearchQueryLimit =
        {queryVector: [1.0, 2.0, 3.0], path: "x", numCandidates: 10, limit: 5};

    const expectedMongotCommandLimit = mongotCommandForVectorSearchQuery(
        {...vectorSearchQueryLimit, collName, dbName, collectionUUID});

    const responseOk = 1;

    const mongot0ResponseBatch = [
        {_id: 3, $vectorSearchScore: 0.99},
        {_id: 2, $vectorSearchScore: 0.10},
        {_id: 4, $vectorSearchScore: 0.01},
        {_id: 1, $vectorSearchScore: 0.0099},
    ];
    const history0 = [{
        expectedCommand: expectedMongotCommandLimit,
        response: mongotResponseForBatch(mongot0ResponseBatch, NumberLong(0), collNS, responseOk),
    }];

    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId);

    const mongot1ResponseBatch = [
        {_id: 11, $vectorSearchScore: 1.0},
        {_id: 13, $vectorSearchScore: 0.30},
        {_id: 12, $vectorSearchScore: 0.29},
        {_id: 14, $vectorSearchScore: 0.28},
    ];
    const history1 = [{
        expectedCommand: expectedMongotCommandLimit,
        response: mongotResponseForBatch(mongot1ResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId);

    const expectedDocs = [
        {_id: 11, shardKey: 100, x: "brown", y: "ipsum"},
        {_id: 3, shardKey: 0, x: "brown", y: "ipsum"},
        {_id: 13, shardKey: 100, x: "brown", y: "ipsum"},
        {_id: 12, shardKey: 100, x: "cow", y: "lorem ipsum"},
        {_id: 14, shardKey: 100, x: "cow", y: "lorem ipsum"},
    ];

    assert.eq(testColl.aggregate({$vectorSearch: vectorSearchQueryLimit}).toArray(), expectedDocs);
}
runTestOnPrimaries(testBasicLimitCase);
runTestOnSecondaries(testBasicLimitCase);

// Tests that $vectorSearch returns the correct number of documents on unbalanced shards.
function testLimitOnUnbalancedShards(shard0Conn, shard1Conn) {
    const vectorSearchQueryUnbalanced =
        {queryVector: [1.0, 2.0, 3.0], path: "x", numCandidates: 10, limit: 3};

    const expectedMongotCommandLimit = mongotCommandForVectorSearchQuery(
        {...vectorSearchQueryUnbalanced, collName, dbName, collectionUUID});

    const responseOk = 1;

    const mongot0ResponseBatch = [
        {_id: 3, $vectorSearchScore: 0.99},
        {_id: 2, $vectorSearchScore: 0.10},
        {_id: 4, $vectorSearchScore: 0.01},
        {_id: 1, $vectorSearchScore: 0.0099},
    ];
    const history0 = [{
        expectedCommand: expectedMongotCommandLimit,
        response: mongotResponseForBatch(mongot0ResponseBatch, NumberLong(0), collNS, responseOk),
    }];

    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId);

    // Set second shard to return nothing.
    const mongot1ResponseBatch = [];
    const history1 = [{
        expectedCommand: expectedMongotCommandLimit,
        response: mongotResponseForBatch(mongot1ResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId);

    const expectedDocs = [
        {_id: 3, shardKey: 0, x: "brown", y: "ipsum"},
        {_id: 2, shardKey: 0, x: "now", y: "lorem"},
        {_id: 4, shardKey: 0, x: "cow", y: "lorem ipsum"},
    ];

    assert.eq(testColl.aggregate({$vectorSearch: vectorSearchQueryUnbalanced}).toArray(),
              expectedDocs);
}
runTestOnPrimaries(testLimitOnUnbalancedShards);
runTestOnSecondaries(testLimitOnUnbalancedShards);

// Tests that $vectorSearch returns an error with a limit of 0 on a sharded cluster.
(function testLimitLessThanOneOnSharded() {
    const vectorSearchQueryLimitOne =
        {queryVector: [1.0, 2.0, 3.0], path: "x", numCandidates: 10, limit: 0};
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: collName,
        pipeline: [{$vectorSearch: vectorSearchQueryLimitOne}],
        cursor: {}
    }),
                                 7912700);
})();

// Test with a vector search filter that gets desugared during serialization.
vectorSearchQuery = {
    "index": "default",
    "path": "x",
    "numCandidates": 10,
    "limit": 100,
    "filter": {"version": {"$gt": 1, "$lte": 4, "$ne": 3}},
    "queryVector": [2.0, 2.0]
};
pipeline = [{$vectorSearch: vectorSearchQuery}, {$project: {x: 1, y: 1}}];
expectedMongotCommand =
    mongotCommandForVectorSearchQuery({...vectorSearchQuery, collName, dbName, collectionUUID});
function testFilterNotCase(shard0Conn, shard1Conn) {
    const responseOk = 1;

    const mongot0ResponseBatch = [
        {_id: 3, $vectorSearchScore: 0.99},
        {_id: 2, $vectorSearchScore: 0.10},
        {_id: 4, $vectorSearchScore: 0.01},
        {_id: 1, $vectorSearchScore: 0.0099},
    ];
    const history0 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotResponseForBatch(mongot0ResponseBatch, NumberLong(0), collNS, responseOk),
    }];

    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId);

    const mongot1ResponseBatch = [
        {_id: 11, $vectorSearchScore: 1.0},
        {_id: 13, $vectorSearchScore: 0.30},
        {_id: 12, $vectorSearchScore: 0.29},
        {_id: 14, $vectorSearchScore: 0.28},
    ];
    const history1 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotResponseForBatch(mongot1ResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId);

    const expectedDocs = [
        {_id: 11, x: "brown", y: "ipsum"},
        {_id: 3, x: "brown", y: "ipsum"},
        {_id: 13, x: "brown", y: "ipsum"},
        {_id: 12, x: "cow", y: "lorem ipsum"},
        {_id: 14, x: "cow", y: "lorem ipsum"},
        {_id: 2, x: "now", y: "lorem"},
        {_id: 4, x: "cow", y: "lorem ipsum"},
        {_id: 1, x: "ow"},
    ];

    assert.eq(testColl.aggregate(pipeline).toArray(), expectedDocs);
}
runTestOnPrimaries(testFilterNotCase);
runTestOnSecondaries(testFilterNotCase);
stWithMock.stop();
