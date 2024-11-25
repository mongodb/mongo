/**
 * queryStats test for sharded $search queries.
 * @tags: [featureFlagQueryStats]
 */

import {
    getQueryStatsAggCmd,
    getQueryStatsServerParameters
} from "jstests/libs/query/query_stats_utils.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
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
    verbose: 5,
    mongos: 1,
    mongosOptions: getQueryStatsServerParameters()
});
stWithMock.start();
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const testColl = testDB.getCollection(collName);
testColl.drop();
const collNS = testColl.getFullName();

assert.commandWorked(testColl.insert({_id: 1, x: "ow"}));
assert.commandWorked(testColl.insert({_id: 2, x: "now", y: "lorem"}));
assert.commandWorked(testColl.insert({_id: 3, x: "brown", y: "ipsum"}));
assert.commandWorked(testColl.insert({_id: 4, x: "cow", y: "lorem ipsum"}));
assert.commandWorked(testColl.insert({_id: 11, x: "brown", y: "ipsum"}));
assert.commandWorked(testColl.insert({_id: 12, x: "cow", y: "lorem ipsum"}));
assert.commandWorked(testColl.insert({_id: 13, x: "brown", y: "ipsum"}));
assert.commandWorked(testColl.insert({_id: 14, x: "cow", y: "lorem ipsum"}));

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
st.shardColl(testColl, {_id: 1}, {_id: 10}, {_id: 10 + 1});

const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), collName);

const mongotQuery = {};
const protocolVersion = NumberInt(1);
const expectedMongotCommand = mongotCommandForQuery({
    query: mongotQuery,
    collName: collName,
    db: dbName,
    collectionUUID: collUUID0,
    protocolVersion: protocolVersion
});

const cursorId = NumberLong(123);
const secondCursorId = NumberLong(cursorId + 1001);
const pipeline = [
    {$search: mongotQuery},
];

function runTestOnPrimaries(testFn) {
    testDB.getMongo().setReadPref("primary");
    testFn(st.rs0.getPrimary(), st.rs1.getPrimary());
}

function runTestOnSecondaries(testFn) {
    testDB.getMongo().setReadPref("secondary");
    testFn(st.rs0.getSecondary(), st.rs1.getSecondary());
}

// Tests that $search returns documents in descending $meta: searchScore.
function testBasicCase(shard0Conn, shard1Conn) {
    const responseOk = 1;

    const mongot0ResponseBatch = [
        {_id: 3, $searchScore: 100},
        {_id: 2, $searchScore: 10},
        {_id: 4, $searchScore: 1},
        {_id: 1, $searchScore: 0.99},
    ];
    const history0 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotMultiCursorResponseForBatch(
            mongot0ResponseBatch, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk),
    }];

    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId, secondCursorId);

    const mongot1ResponseBatch = [
        {_id: 11, $searchScore: 111},
        {_id: 13, $searchScore: 30},
        {_id: 12, $searchScore: 29},
        {_id: 14, $searchScore: 28},
    ];
    const history1 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotMultiCursorResponseForBatch(
            mongot1ResponseBatch, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId, secondCursorId);

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

    mockPlanShardedSearchResponse(
        testColl.getName(), mongotQuery, dbName, undefined /*sortSpec*/, stWithMock);

    assert.eq(testColl.aggregate(pipeline).toArray(), expectedDocs);
}
runTestOnPrimaries(testBasicCase);
let queryStats = getQueryStatsAggCmd(testDB);
assert.eq(1, queryStats.length, queryStats);

runTestOnSecondaries(testBasicCase);
queryStats = getQueryStatsAggCmd(testDB);
// Though executed twice (once on primaries and once on secondaries), the $search queries will have
// the same shape. The second key will bef or the $queryStats command ran above.
assert.eq(2, queryStats.length, queryStats);
stWithMock.stop();
