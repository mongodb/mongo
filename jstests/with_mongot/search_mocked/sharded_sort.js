/**
 * Sharding tests where $search is given a sort.
 *
 * @tags: [
 *     requires_fcv_70
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getAggPlanStages, getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    getDefaultProtocolVersionForPlanShardedSearch,
    mockPlanShardedSearchResponse,
    mongotCommandForQuery,
    mongotMultiCursorResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = jsTestName();

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
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
stWithMock.assertEmptyMocks();
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const testColl = testDB.getCollection(collName);
const collNS = testColl.getFullName();
const protocolVersion = getDefaultProtocolVersionForPlanShardedSearch();

assert.commandWorked(testColl.insert([
    {_id: 1, x: "ow", z: 20, a: {b: 20}, c: 1, d: [0, 5, 10]},
    {_id: 2, x: "now", y: "lorem", z: 10, a: {b: 10}, c: 10, d: -1},
    {_id: 3, x: "brown", y: "ipsum", z: 5, a: {b: 5}, e: ISODate("2020-01-01T01:00:00Z")},
    {_id: 4, x: "cow", y: "lorem ipsum", z: 15, a: {b: 15}, c: 8},
    {_id: 11, x: "brown", y: "ipsum", z: 6, a: {b: 6}, c: 3},
    {
        _id: 12,
        x: "cow",
        y: "lorem ipsum",
        z: 28,
        a: {b: 28},
        c: 30,
        e: ISODate("2021-01-01T01:00:00Z")
    },
    {_id: 13, x: "brown", y: "ipsum", z: 12, a: {b: 12}, c: 0},
    {_id: 14, x: "cow", y: "lorem ipsum", z: 30, a: {b: 30}, c: 4},
    {_id: 15, x: "crown", y: "ipsum", z: 5, a: {b: 5}, c: 5, d: [3, 20]}
]));

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
st.shardColl(testColl, {_id: 1}, {_id: 10}, {_id: 10 + 1});

const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), collName);

const cursorId = NumberLong(123);
const secondCursorId = NumberLong(cursorId + 1001);

let shard0Conn = st.rs0.getPrimary();
let shard1Conn = st.rs1.getPrimary();

/**
 * Helper function to set the mock responses for the two shards' mongots.
 * @param {Object} mongotQuery
 * @param {Array<Object>} shard0MockResponse
 * @param {Array<Object>} shard1MockResponse
 */
function mockMongotShardResponses(mongotQuery, shard0MockResponse, shard1MockResponse) {
    const responseOk = 1;
    const history0 = [{
        expectedCommand: mongotCommandForQuery({
            query: mongotQuery,
            collName: collName,
            db: dbName,
            collectionUUID: collUUID0,
            protocolVersion: protocolVersion
        }),
        response: mongotMultiCursorResponseForBatch(
            shard0MockResponse, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk),
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId, secondCursorId);

    const history1 = [{
        expectedCommand: mongotCommandForQuery({
            query: mongotQuery,
            collName: collName,
            db: dbName,
            collectionUUID: collUUID1,
            protocolVersion: protocolVersion
        }),
        response: mongotMultiCursorResponseForBatch(
            shard1MockResponse, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId, secondCursorId);
}

(function testSingleValueSort() {
    // Note: All mongot queries in this test file are only used by the mock for the purposes of
    // keying a mocked response, they may use incorrect syntax.
    const mongotQuery = {
        sort: {z: 1},
    };
    const sortSpec = {
        "$searchSortValues.z": 1,
    };
    mockPlanShardedSearchResponse(testColl.getName(), mongotQuery, dbName, sortSpec, stWithMock);

    const mongot0ResponseBatch = [
        {_id: 3, $searchScore: 1, $searchSortValues: {z: 5}},
        {_id: 2, $searchScore: 0.99, $searchSortValues: {z: 10}},
        {_id: 4, $searchScore: 100, $searchSortValues: {z: 15}},
        {_id: 1, $searchScore: 10, $searchSortValues: {z: 20}},
    ];
    const mongot1ResponseBatch = [
        {_id: 15, $searchScore: 2, $searchSortValues: {z: 5}},
        {_id: 11, $searchScore: 29, $searchSortValues: {z: 6}},
        {_id: 13, $searchScore: 30, $searchSortValues: {z: 12}},
        {_id: 12, $searchScore: 111, $searchSortValues: {z: 28}},
        {_id: 14, $searchScore: 28, $searchSortValues: {z: 30}},
    ];
    mockMongotShardResponses(mongotQuery, mongot0ResponseBatch, mongot1ResponseBatch);

    const expectedDocs = [
        {z: 5},
        {z: 5},
        {z: 6},
        {z: 10},
        {z: 12},
        {z: 15},
        {z: 20},
        {z: 28},
        {z: 30},
    ];

    assert.eq(expectedDocs,
              testColl
                  .aggregate([
                      {$search: mongotQuery},
                      // Project out '_id' as there are multiple entries with the same 'z' value
                      // and we need the results to be hermetic.
                      {$project: {_id: 0, z: 1}},
                  ])
                  .toArray());
})();

(function testDescendingSort() {
    const mongotQuery = {
        sort: {z: -1},
    };
    const sortSpec = {
        "$searchSortValues.z": -1,
    };
    mockPlanShardedSearchResponse(testColl.getName(), mongotQuery, dbName, sortSpec, stWithMock);

    const mongot0ResponseBatch = [
        {_id: 1, $searchScore: 10, $searchSortValues: {z: 20}},
        {_id: 4, $searchScore: 100, $searchSortValues: {z: 15}},
        {_id: 2, $searchScore: 0.99, $searchSortValues: {z: 10}},
        {_id: 3, $searchScore: 1, $searchSortValues: {z: 5}},
    ];
    const mongot1ResponseBatch = [
        {_id: 14, $searchScore: 28, $searchSortValues: {z: 30}},
        {_id: 12, $searchScore: 111, $searchSortValues: {z: 28}},
        {_id: 13, $searchScore: 30, $searchSortValues: {z: 12}},
        {_id: 11, $searchScore: 29, $searchSortValues: {z: 6}},
        {_id: 15, $searchScore: 2, $searchSortValues: {z: 5}},
    ];
    mockMongotShardResponses(mongotQuery, mongot0ResponseBatch, mongot1ResponseBatch);

    const expectedDocs = [
        {z: 30},
        {z: 28},
        {z: 20},
        {z: 15},
        {z: 12},
        {z: 10},
        {z: 6},
        {z: 5},
        {z: 5},
    ];

    assert.eq(expectedDocs,
              testColl
                  .aggregate([
                      {$search: mongotQuery},
                      // Project out '_id' as there are multiple entries with the same 'z' value
                      // and we need the results to be hermetic.
                      {$project: {_id: 0, z: 1}},
                  ])
                  .toArray());
})();

(function testCompoundSort() {
    const mongotQuery = {
        sort: {z: 1, x: 1},
    };
    const sortSpec = {
        "$searchSortValues.z": 1,
        "$searchSortValues.x": 1,
    };
    mockPlanShardedSearchResponse(testColl.getName(), mongotQuery, dbName, sortSpec, stWithMock);

    const mongot0ResponseBatch = [
        // Vary order here to ensure that we are not depending on the field order of
        // $searchSortValues.
        {_id: 3, $searchScore: 1, $searchSortValues: {z: 5, x: "brown"}},
        {_id: 2, $searchScore: 0.99, $searchSortValues: {z: 10, x: "now"}},
        {_id: 4, $searchScore: 100, $searchSortValues: {x: "cow", z: 15}},
        {_id: 1, $searchScore: 10, $searchSortValues: {z: 20, x: "ow"}},
    ];
    const mongot1ResponseBatch = [
        {_id: 15, $searchScore: 2, $searchSortValues: {z: 5, x: "crown"}},
        {_id: 11, $searchScore: 29, $searchSortValues: {z: 6, x: "brown"}},
        {_id: 13, $searchScore: 30, $searchSortValues: {x: "brown", z: 12}},
        {_id: 12, $searchScore: 111, $searchSortValues: {z: 28, x: "cow"}},
        {_id: 14, $searchScore: 28, $searchSortValues: {z: 30, x: "cow"}},
    ];
    mockMongotShardResponses(mongotQuery, mongot0ResponseBatch, mongot1ResponseBatch);

    const expectedDocs = [
        {_id: 3, x: "brown", z: 5},  // Duplicate z values are sorted by x values.
        {_id: 15, x: "crown", z: 5},
        {_id: 11, x: "brown", z: 6},
        {_id: 2, x: "now", z: 10},
        {_id: 13, x: "brown", z: 12},
        {_id: 4, x: "cow", z: 15},
        {_id: 1, x: "ow", z: 20},
        {_id: 12, x: "cow", z: 28},
        {_id: 14, x: "cow", z: 30},
    ];

    assert.eq(expectedDocs,
              testColl
                  .aggregate([
                      {$search: mongotQuery},
                      {$project: {_id: 1, x: 1, z: 1}},
                  ])
                  .toArray());
})();

(function testDottedPathSort() {
    const mongotQuery = {
        sort: {"a.b": 1},
    };
    const sortSpec = {
        "$searchSortValues.a_b": 1,
    };
    mockPlanShardedSearchResponse(testColl.getName(), mongotQuery, dbName, sortSpec, stWithMock);

    const mongot0ResponseBatch = [
        {_id: 3, $searchScore: 1, $searchSortValues: {"a_b": 5}},
        {_id: 2, $searchScore: 0.99, $searchSortValues: {"a_b": 10}},
        {_id: 4, $searchScore: 100, $searchSortValues: {"a_b": 15}},
        {_id: 1, $searchScore: 10, $searchSortValues: {"a_b": 20}},
    ];
    const mongot1ResponseBatch = [
        {_id: 15, $searchScore: 2, $searchSortValues: {"a_b": 5}},
        {_id: 11, $searchScore: 29, $searchSortValues: {"a_b": 6}},
        {_id: 13, $searchScore: 30, $searchSortValues: {"a_b": 12}},
        {_id: 12, $searchScore: 111, $searchSortValues: {"a_b": 28}},
        {_id: 14, $searchScore: 28, $searchSortValues: {"a_b": 30}},
    ];
    mockMongotShardResponses(mongotQuery, mongot0ResponseBatch, mongot1ResponseBatch);

    const expectedDocs = [
        {a: {b: 5}},
        {a: {b: 5}},
        {a: {b: 6}},
        {a: {b: 10}},
        {a: {b: 12}},
        {a: {b: 15}},
        {a: {b: 20}},
        {a: {b: 28}},
        {a: {b: 30}},
    ];

    assert.eq(expectedDocs,
              testColl
                  .aggregate([
                      {$search: mongotQuery},
                      {$project: {_id: 0, "a.b": 1}},
                  ])
                  .toArray());
})();

(function testMissingValue() {
    const mongotQuery = {
        sort: {c: 1},
    };
    const sortSpec = {
        "$searchSortValues.c": 1,
    };
    mockPlanShardedSearchResponse(testColl.getName(), mongotQuery, dbName, sortSpec, stWithMock);

    const mongot0ResponseBatch = [
        // mongot is allowed to omit a sort value if the field is missing or of an unindexable type.
        // This test case verifies that mongod properly inserts a null sortKey.
        {_id: 3, $searchScore: 1, $searchSortValues: {}},
        {_id: 1, $searchScore: 10, $searchSortValues: {c: 1}},
        {_id: 4, $searchScore: 100, $searchSortValues: {c: 8}},
        {_id: 2, $searchScore: 0.99, $searchSortValues: {c: 10}},
    ];
    const mongot1ResponseBatch = [
        {_id: 13, $searchScore: 30, $searchSortValues: {c: 0}},
        {_id: 11, $searchScore: 29, $searchSortValues: {c: 3}},
        {_id: 14, $searchScore: 28, $searchSortValues: {c: 4}},
        {_id: 15, $searchScore: 2, $searchSortValues: {c: 5}},
        {_id: 12, $searchScore: 111, $searchSortValues: {c: 30}},
    ];
    mockMongotShardResponses(mongotQuery, mongot0ResponseBatch, mongot1ResponseBatch);

    const expectedDocs = [
        {_id: 3},
        {_id: 13, c: 0},
        {_id: 1, c: 1},
        {_id: 11, c: 3},
        {_id: 14, c: 4},
        {_id: 15, c: 5},
        {_id: 4, c: 8},
        {_id: 2, c: 10},
        {_id: 12, c: 30},
    ];

    assert.eq(expectedDocs,
              testColl
                  .aggregate([
                      {$search: mongotQuery},
                      {$project: {_id: 1, c: 1}},
                  ])
                  .toArray());
})();

(function testStoredSource() {
    const mongotQuery = {
        sort: {c: 1},
        returnStoredSource: true,
    };
    const sortSpec = {
        "$searchSortValues.c": 1,
    };
    mockPlanShardedSearchResponse(testColl.getName(), mongotQuery, dbName, sortSpec, stWithMock);

    const mongot0ResponseBatch = [
        {storedSource: {_id: 3}, $searchScore: 1, $searchSortValues: {}},
        {storedSource: {_id: 1, c: 1}, $searchScore: 10, $searchSortValues: {c: 1}},
        {storedSource: {_id: 4, c: 8}, $searchScore: 100, $searchSortValues: {c: 8}},
        {storedSource: {_id: 2, c: 10}, $searchScore: 0.99, $searchSortValues: {c: 10}},
    ];
    const mongot1ResponseBatch = [
        {storedSource: {_id: 13, c: 0}, $searchScore: 30, $searchSortValues: {c: 0}},
        {storedSource: {_id: 11, c: 3}, $searchScore: 29, $searchSortValues: {c: 3}},
        {storedSource: {_id: 14, c: 4}, $searchScore: 28, $searchSortValues: {c: 4}},
        {storedSource: {_id: 15, c: 5}, $searchScore: 2, $searchSortValues: {c: 5}},
        {storedSource: {_id: 12, c: 30}, $searchScore: 111, $searchSortValues: {c: 30}},
    ];
    mockMongotShardResponses(mongotQuery, mongot0ResponseBatch, mongot1ResponseBatch);

    const expectedDocs = [
        {_id: 3},
        {_id: 13, c: 0},
        {_id: 1, c: 1},
        {_id: 11, c: 3},
        {_id: 14, c: 4},
        {_id: 15, c: 5},
        {_id: 4, c: 8},
        {_id: 2, c: 10},
        {_id: 12, c: 30},
    ];

    assert.eq(expectedDocs,
              testColl
                  .aggregate([
                      {$search: mongotQuery},
                  ])
                  .toArray());
})();

(function testArraysSort() {
    const mongotQuery = {
        sort: {d: 1},
    };
    // The 'd' field might contain an array. It's $searchSortValue depends on the direction of the
    // sort that is specified on that field.
    const sortSpec = {
        "$searchSortValues.d": 1,
    };
    mockPlanShardedSearchResponse(testColl.getName(), mongotQuery, dbName, sortSpec, stWithMock);

    const mongot0ResponseBatch = [
        {_id: 3, $searchScore: 1, $searchSortValues: {d: null}},
        {_id: 4, $searchScore: 100, $searchSortValues: {d: null}},
        {_id: 2, $searchScore: 0.99, $searchSortValues: {d: -1}},
        {_id: 1, $searchScore: 10, $searchSortValues: {d: 0}},
    ];
    const mongot1ResponseBatch = [
        {_id: 11, $searchScore: 29, $searchSortValues: {d: null}},
        {_id: 12, $searchScore: 111, $searchSortValues: {d: null}},
        {_id: 13, $searchScore: 30, $searchSortValues: {d: null}},
        {_id: 14, $searchScore: 28, $searchSortValues: {d: null}},
        {_id: 15, $searchScore: 2, $searchSortValues: {d: 3}},
    ];
    mockMongotShardResponses(mongotQuery, mongot0ResponseBatch, mongot1ResponseBatch);

    const expectedDocs = [
        {_id: 2, d: -1},
        {_id: 1, d: [0, 5, 10]},
        {_id: 15, d: [3, 20]},
    ];

    assert.eq(expectedDocs,
              testColl
                  .aggregate([
                      {$search: mongotQuery},
                      {$match: {d: {$ne: null}}},
                      {$project: {_id: 1, d: 1}},
                  ])
                  .toArray());
})();

(function testDateSort() {
    const mongotQuery = {
        sort: {e: 1},
    };
    const sortSpec = {
        "$searchSortValues.e": 1,
    };
    mockPlanShardedSearchResponse(testColl.getName(), mongotQuery, dbName, sortSpec, stWithMock);

    const mongot0ResponseBatch = [
        {_id: 1, $searchScore: 10, $searchSortValues: {e: null}},
        {_id: 2, $searchScore: 0.99, $searchSortValues: {e: null}},
        {_id: 4, $searchScore: 100, $searchSortValues: {e: null}},
        {_id: 3, $searchScore: 1, $searchSortValues: {e: ISODate("2020-01-01T01:00:00Z")}},
    ];
    const mongot1ResponseBatch = [
        {_id: 11, $searchScore: 29, $searchSortValues: {e: null}},
        {_id: 13, $searchScore: 30, $searchSortValues: {e: null}},
        {_id: 14, $searchScore: 28, $searchSortValues: {e: null}},
        {_id: 15, $searchScore: 2, $searchSortValues: {e: null}},
        {_id: 12, $searchScore: 111, $searchSortValues: {e: ISODate("2021-01-01T01:00:00Z")}},
    ];
    mockMongotShardResponses(mongotQuery, mongot0ResponseBatch, mongot1ResponseBatch);

    const expectedDocs = [
        {_id: 3, e: ISODate("2020-01-01T01:00:00Z")},
        {_id: 12, e: ISODate("2021-01-01T01:00:00Z")},
    ];

    assert.eq(expectedDocs,
              testColl
                  .aggregate([
                      {$search: mongotQuery},
                      {$match: {e: {$ne: null}}},
                      {$project: {_id: 1, e: 1}},
                  ])
                  .toArray());
})();

(function testGetMoreOnShard() {
    const mongotQuery = {
        sort: {z: 1},
    };
    const sortSpec = {
        "$searchSortValues.z": 1,
    };
    mockPlanShardedSearchResponse(testColl.getName(), mongotQuery, dbName, sortSpec, stWithMock);

    const responseOk = {ok: 1};

    // Mock response from shard 0's mongot.
    const mongot0ResponseBatch = [
        {_id: 3, $searchScore: 1, $searchSortValues: {z: 5}},
        {_id: 2, $searchScore: 0.99, $searchSortValues: {z: 10}},
        {_id: 4, $searchScore: 100, $searchSortValues: {z: 15}},
        {_id: 1, $searchScore: 10, $searchSortValues: {z: 20}},
    ];
    const history0 = [
        {
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID0,
                protocolVersion: protocolVersion
            }),
            response: mongotMultiCursorResponseForBatch(mongot0ResponseBatch.slice(0, 2),
                                                        NumberLong(10),
                                                        [{val: 1}],
                                                        NumberLong(20),
                                                        collNS,
                                                        responseOk),
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
    const historyMeta0 = [
        {
            expectedCommand: {getMore: NumberLong(20), collection: collName},
            response: {cursor: {id: NumberLong(0), ns: collNS, nextBatch: [{metaVal: 1}]}, ok: 1},
            maybeUnused: true,
        },
    ];

    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, NumberLong(10));
    s0Mongot.setMockResponses(historyMeta0, NumberLong(20));

    // Mock response from shard 1's mongot.
    const mongot1ResponseBatch = [
        {_id: 15, $searchScore: 2, $searchSortValues: {z: 5}},
        {_id: 11, $searchScore: 29, $searchSortValues: {z: 6}},
        {_id: 13, $searchScore: 30, $searchSortValues: {z: 12}},
        {_id: 12, $searchScore: 111, $searchSortValues: {z: 28}},
        {_id: 14, $searchScore: 28, $searchSortValues: {z: 30}},
    ];
    const history1 = [
        {
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID1,
                protocolVersion: protocolVersion
            }),
            response: mongotMultiCursorResponseForBatch(mongot1ResponseBatch.slice(0, 1),
                                                        NumberLong(30),
                                                        [{val: 1}],
                                                        NumberLong(40),
                                                        collNS,
                                                        responseOk),
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
    const historyMeta1 = [
        {
            expectedCommand: {getMore: NumberLong(40), collection: collName},
            response: {cursor: {id: NumberLong(0), ns: collNS, nextBatch: [{metaVal: 1}]}, ok: 1},
            maybeUnused: true,
        },
    ];

    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, NumberLong(30));
    s1Mongot.setMockResponses(historyMeta1, NumberLong(40));

    const expectedDocs = [
        {z: 5},
        {z: 5},
        {z: 6},
        {z: 10},
        {z: 12},
        {z: 15},
        {z: 20},
        {z: 28},
        {z: 30},
    ];

    assert.eq(expectedDocs,
              testColl
                  .aggregate([
                      {$search: mongotQuery},
                      {$project: {_id: 0, z: 1}},
                  ])
                  .toArray());
})();

(function testSearchScoreSpec() {
    const mongotQuery = {};
    const sortSpec = {
        "$searchScore": -1,
    };
    mockPlanShardedSearchResponse(testColl.getName(), mongotQuery, dbName, sortSpec, stWithMock);

    const mongot0ResponseBatch = [
        {_id: 4, $searchScore: 100},
        {_id: 1, $searchScore: 10},
        {_id: 3, $searchScore: 1},
        {_id: 2, $searchScore: 0.99},
    ];
    const mongot1ResponseBatch = [
        {_id: 12, $searchScore: 111},
        {_id: 13, $searchScore: 30},
        {_id: 11, $searchScore: 29},
        {_id: 14, $searchScore: 28},
        {_id: 15, $searchScore: 2},
    ];
    mockMongotShardResponses(mongotQuery, mongot0ResponseBatch, mongot1ResponseBatch);

    const expectedDocs = [
        {_id: 12, score: 111},
        {_id: 4, score: 100},
        {_id: 13, score: 30},
        {_id: 11, score: 29},
        {_id: 14, score: 28},
        {_id: 1, score: 10},
        {_id: 15, score: 2},
        {_id: 3, score: 1},
        {_id: 2, score: 0.99},
    ];

    assert.eq(expectedDocs,
              testColl
                  .aggregate([
                      {$search: mongotQuery},
                      {$project: {_id: 1, score: {$meta: "searchScore"}}},
                  ])
                  .toArray());
})();

(function testExplain() {
    const explainContents = {explain: "searching"};
    const mongotQuery = {
        sort: {z: 1},
    };
    const sortSpec = {
        "$searchSortValues.z": 1,
    };
    const planShardedSearchHistory = [{
        expectedCommand: {
            planShardedSearch: collName,
            query: mongotQuery,
            $db: dbName,
            explain: {verbosity: "queryPlanner"},
            searchFeatures: {shardedSort: 1}
        },
        response: {
            ok: 1,
            protocolVersion: protocolVersion,
            metaPipeline: [],
            sortSpec: sortSpec,
        }
    }];
    stWithMock.getMockConnectedToHost(stWithMock.st.s)
        .setMockResponses(planShardedSearchHistory, cursorId);

    const history0 = [{
        expectedCommand: {
            search: collName,
            collectionUUID: collUUID0,
            query: mongotQuery,
            explain: {verbosity: "queryPlanner"},
            $db: dbName
        },
        response: {explain: explainContents, ok: 1},
    }];

    const history1 = [{
        expectedCommand: {
            search: collName,
            collectionUUID: collUUID1,
            query: mongotQuery,
            explain: {verbosity: "queryPlanner"},
            $db: dbName
        },
        response: {explain: explainContents, ok: 1},
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId);
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId);

    const result = testColl.explain().aggregate([{$search: mongotQuery}]);
    if (checkSbeRestrictedOrFullyEnabled(testDB) &&
        FeatureFlagUtil.isPresentAndEnabled(testDB.getMongo(), 'SearchInSbe')) {
        const winningPlan = getQueryPlanner(result.shards[st.shard0.shardName]).winningPlan;
        assert(winningPlan.hasOwnProperty('remotePlans'));
        assert.eq(1, winningPlan.remotePlans.length, winningPlan);
        const remotePlan = winningPlan.remotePlans[0];
        assert.eq(explainContents, remotePlan.explain, remotePlan);
        assert.eq(sortSpec, remotePlan.sortSpec, remotePlan);
    } else {
        const searchStages = getAggPlanStages(result, "$_internalSearchMongotRemote");

        assert.eq(2, searchStages.length);

        for (const stage of searchStages) {
            assert.eq(explainContents, stage["$_internalSearchMongotRemote"]["explain"]);
            assert.eq(sortSpec, stage["$_internalSearchMongotRemote"]["sortSpec"]);
        }
    }
})();

stWithMock.stop();
