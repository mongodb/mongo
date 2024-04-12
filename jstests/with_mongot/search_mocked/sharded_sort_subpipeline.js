/**
 * Sharding tests where $search with a sort is invoked as part of a subpipeline.
 *
 * @tags: [
 *     requires_fcv_70
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mockPlanShardedSearchResponseOnConn,
    mongotCommandForQuery,
    mongotMultiCursorResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const unshardedCollName = jsTestName() + "_unsharded";
const shardedCollName = jsTestName() + "_sharded";

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
    shards: {
        rs0: {nodes: 1},
        rs1: {nodes: 1},
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

const shardedColl = testDB.getCollection(shardedCollName);

assert.commandWorked(shardedColl.insert([
    {_id: 1, a: 10},
    {_id: 2, a: 20},
    {_id: 3, a: 15},
    {_id: 4, a: 5},
    {_id: 11, a: 50},
    {_id: 12, a: 0},
    {_id: 13, a: 22},
    {_id: 14, a: 3},
]));

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
st.shardColl(shardedColl, {_id: 1}, {_id: 10}, {_id: 10 + 1});

const shard0MockResponse = [
    {_id: 4, $searchSortValues: {a: 5}},
    {_id: 1, $searchSortValues: {a: 10}},
    {_id: 3, $searchSortValues: {a: 15}},
    {_id: 2, $searchSortValues: {a: 20}},
];
const shard1MockResponse = [
    {_id: 12, $searchSortValues: {a: 0}},
    {_id: 14, $searchSortValues: {a: 3}},
    {_id: 13, $searchSortValues: {a: 22}},
    {_id: 11, $searchSortValues: {a: 50}},
];

const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), shardedCollName);
const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), shardedCollName);

let shard0Conn = st.rs0.getPrimary();
let shard1Conn = st.rs1.getPrimary();

const unshardedColl = testDB.getCollection(unshardedCollName);
unshardedColl.drop();

assert.commandWorked(unshardedColl.insert([
    {b: 1},
    {b: 2},
    {b: 3},
]));

var uid = NumberLong(1432);
function uniqueCursorId() {
    return uid++;
}

function mockMongotShardResponses(mongotQuery) {
    const responseOk = 1;
    const history0 = [{
        expectedCommand: mongotCommandForQuery(mongotQuery, shardedCollName, dbName, collUUID0),
        response: mongotMultiCursorResponseForBatch(shard0MockResponse,
                                                    NumberLong(0),
                                                    [{val: 1}],
                                                    NumberLong(0),
                                                    shardedColl.getFullName(),
                                                    responseOk),
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, uniqueCursorId(), uniqueCursorId());

    const history1 = [{
        expectedCommand: mongotCommandForQuery(mongotQuery, shardedCollName, dbName, collUUID1),
        response: mongotMultiCursorResponseForBatch(shard1MockResponse,
                                                    NumberLong(0),
                                                    [{val: 1}],
                                                    NumberLong(0),
                                                    shardedColl.getFullName(),
                                                    responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, uniqueCursorId(), uniqueCursorId());
}

(function testUnionWith() {
    const mongotQuery = {sort: {a: 1}};
    const sortSpec = {"$searchSortValues.a": 1};
    mockPlanShardedSearchResponseOnConn(
        shardedCollName, mongotQuery, dbName, sortSpec, stWithMock, shard0Conn);
    mockMongotShardResponses(mongotQuery);

    const result = unshardedColl
                       .aggregate([
                           {$match: {b: {$lt: 3}}},
                           {$project: {_id: 0}},
                           {
                               $unionWith: {
                                   coll: shardedCollName,
                                   pipeline: [
                                       {$search: {sort: {a: 1}}},
                                       {$project: {_id: 0}},
                                   ],
                               }
                           },
                       ])
                       .toArray();

    assert.sameMembers(
        [
            {b: 1},
            {b: 2},
            {a: 0},
            {a: 3},
            {a: 5},
            {a: 10},
            {a: 15},
            {a: 20},
            {a: 22},
            {a: 50},
        ],
        result);
})();

(function testLookupUnshardedLocalShardedForeign() {
    const mongotQuery = {sort: {a: 1}};
    const sortSpec = {"$searchSortValues.a": 1};
    // Mock PSS on primary shard's mongotmock.
    // The primary shard ends up sending planShardedSearch to its mongotmock during $lookup's
    // getNext() implementation which parses the subpipeline on each invocation.
    mockPlanShardedSearchResponseOnConn(
        shardedCollName, mongotQuery, dbName, sortSpec, stWithMock, shard0Conn);
    mockMongotShardResponses(mongotQuery);
    const result =
        unshardedColl
            .aggregate([
                {$match: {b: 1}}, // Ensure only one element from the local collection.
                {$project: {_id: 0}},
                {$lookup: {from: shardedCollName, pipeline: [
                    {$search: {sort: {a: 1}}},
                    {$match: {a: {$lt: 4}}},
                    {$project: {_id: 0}},
                ], as: "out"}},
            ])
            .toArray();
    assert.eq([{b: 1, out: [{a: 0}, {a: 3}]}], result);
})();

(function testLookupBothLocalAndForeignSharded() {
    const mongotQuery = {sort: {a: 1}};
    const sortSpec = {"$searchSortValues.a": 1};
    // Mock PSS for both shards. It is invoked during the execution of $lookup.
    mockPlanShardedSearchResponseOnConn(
        shardedCollName, mongotQuery, dbName, sortSpec, stWithMock, shard0Conn);
    mockPlanShardedSearchResponseOnConn(
        shardedCollName, mongotQuery, dbName, sortSpec, stWithMock, shard1Conn);
    // Mock search result for both shards twice. Each shard will execute the subpipeline which
    // requires it to get search results for itself and the other shard.
    for (let i = 0; i < 2; i++) {
        mockMongotShardResponses(mongotQuery);
    }
    // Disable the order checking on the shards' mongotmock. This is to prevent a race condition
    // between shards. Mongotmock's default behavior is to enforce the enforce of mocked responses.
    // However, in this query, there is a chance that shard0 starts executing the inner pipeline and
    // asks shard1 to return its search results before shard1 has sent the PSS for it's own
    // execution of the inner pipeline. As a result, we cannot guarentee whether either shard will
    // run PSS or 'search' first, and as a result must disable the order checks.
    stWithMock.getMockConnectedToHost(shard0Conn).disableOrderCheck();
    stWithMock.getMockConnectedToHost(shard1Conn).disableOrderCheck();
    const result =
        shardedColl
            .aggregate([
                {$match: {a: {$in: [0, 5]}}}, // Ensure the pipeline needs to run on both shards.
                {$sort: {a: 1}},
                {$project: {_id: 0}},
                {$lookup: {from: shardedCollName, pipeline: [
                    {$search: {sort: {a: 1}}},
                    {$match: {a: {$lt: 4}}},
                    {$project: {_id: 0}},
                ], as: "out"}},
            ])
            .toArray();
    assert.eq([{a: 0, out: [{a: 0}, {a: 3}]}, {a: 5, out: [{a: 0}, {a: 3}]}], result);
})();

stWithMock.stop();
