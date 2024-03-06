/**
 * Checks that the requiresSearchMetaCursor is set properly on search requests dispatched from
 * mongos to mongod by running search queries and checking the shards' system.profile collection.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mockPlanShardedSearchResponse,
    mockPlanShardedSearchResponseOnConn,
    mongotCommandForQuery,
    mongotMultiCursorResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const shardedCollName = "sharded_coll";
const unshardedCollName = "unsharded_coll";

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
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

// Create a sharded collection to be used for $search and a separate unsharded collection.
const shardedColl = testDB.getCollection(shardedCollName);
const shardedCollNS = shardedColl.getFullName();
const unshardedColl = testDB.getCollection(unshardedCollName);
assert.commandWorked(shardedColl.insert([
    {_id: 1, x: "ow"},
    {_id: 2, x: "now", y: "lorem"},
    {_id: 3, x: "brown", y: "ipsum"},
    {_id: 4, x: "cow", y: "lorem ipsum"},
    {_id: 11, x: "brown", y: "ipsum"},
    {_id: 12, x: "cow", y: "lorem ipsum"},
    {_id: 13, x: "brown", y: "ipsum"},
    {_id: 14, x: "cow", y: "lorem ipsum"}
]));
assert.commandWorked(unshardedColl.insert([{b: 1}, {b: 3}, {b: 5}]));

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
st.shardColl(shardedColl, {_id: 1}, {_id: 10}, {_id: 10 + 1});

const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), shardedCollName);
const mongotQuery = {};
const protocolVersion = NumberInt(1);
const expectedMongotCommand =
    mongotCommandForQuery(mongotQuery, shardedCollName, dbName, collUUID0, protocolVersion);

const cursorId = NumberLong(123);
const metaCursorId = NumberLong(cursorId + 1001);
const responseOk = 1;

const shard0Conn = st.rs0.getPrimary();
const shard0DB = shard0Conn.getDB(dbName);
const shard1Conn = st.rs1.getPrimary();
const shard1DB = shard1Conn.getDB(dbName);

function mockShards() {
    // Mock shard0 responses.
    const mongot0ResponseBatch = [
        {_id: 3, $searchScore: 100},
        {_id: 2, $searchScore: 10},
        {_id: 4, $searchScore: 1},
        {_id: 1, $searchScore: 0.99},
    ];
    const history0 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotMultiCursorResponseForBatch(mongot0ResponseBatch,
                                                    NumberLong(0),
                                                    [{val: 1}],
                                                    NumberLong(0),
                                                    shardedCollNS,
                                                    responseOk),
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId, metaCursorId);

    // Mock shard1 responses.
    const mongot1ResponseBatch = [
        {_id: 11, $searchScore: 111},
        {_id: 13, $searchScore: 30},
        {_id: 12, $searchScore: 29},
        {_id: 14, $searchScore: 28},
    ];
    const history1 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotMultiCursorResponseForBatch(mongot1ResponseBatch,
                                                    NumberLong(0),
                                                    [{val: 1}],
                                                    NumberLong(0),
                                                    shardedCollNS,
                                                    responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId, metaCursorId);
}

function resetShardProfilers() {
    for (let shardDB of [shard0DB, shard1DB]) {
        shardDB.setProfilingLevel(0);
        shardDB.system.profile.drop();
        shardDB.setProfilingLevel(2);
    }
}

// Sets up the mock responses, runs the given pipeline, then confirms via system.profile that the
// requiresSearchMetaCursor field is set properly when commands are dispatched to shards.
function runRequiresSearchMetaCursorTest(
    {pipeline, coll, expectedDocs, shouldRequireSearchMetaCursor, hasSearchMetaStage = false}) {
    // Mock planShardedSearch responses.
    if (coll.getName() === shardedCollName) {
        mockPlanShardedSearchResponse(
            shardedColl.getName(), mongotQuery, dbName, undefined /*sortSpec*/, stWithMock);
    } else {
        // For queries where $search is in the subpipeline, we mock planShardedSearch on the shard
        // that has the unsharded collection, rather than mongos.
        mockPlanShardedSearchResponseOnConn(shardedColl.getName(),
                                            mongotQuery,
                                            dbName,
                                            undefined /*sortSpec*/,
                                            stWithMock,
                                            shard0Conn);
    }

    mockShards();
    resetShardProfilers();

    const comment = "search_query";
    // Run the query and ensure metrics are as expected.
    assert.eq(coll.aggregate(pipeline, {comment}).toArray(), expectedDocs);

    for (let shardDB of [shard0DB, shard1DB]) {
        const res = shardDB.system.profile
                        .find({"command.comment": comment, "command.aggregate": shardedCollName})
                        .toArray();
        assert.eq(1, res.length, res);
        if (hasSearchMetaStage) {
            assert.eq(shouldRequireSearchMetaCursor,
                      res[0].command.pipeline[0].$searchMeta.requiresSearchMetaCursor,
                      res);
        } else {
            assert.eq(shouldRequireSearchMetaCursor,
                      res[0].command.pipeline[0].$search.requiresSearchMetaCursor,
                      res);
        }
    }
}

// Now we run various queries and check that shouldRequireSearchMetaCursor is set appropriately.
runRequiresSearchMetaCursorTest({
    pipeline: [{$search: mongotQuery}],
    coll: shardedColl,
    expectedDocs: [
        {_id: 11, x: "brown", y: "ipsum"},
        {_id: 3, x: "brown", y: "ipsum"},
        {_id: 13, x: "brown", y: "ipsum"},
        {_id: 12, x: "cow", y: "lorem ipsum"},
        {_id: 14, x: "cow", y: "lorem ipsum"},
        {_id: 2, x: "now", y: "lorem"},
        {_id: 4, x: "cow", y: "lorem ipsum"},
        {_id: 1, x: "ow"},
    ],
    hasSearchMetaStage: false,
    shouldRequireSearchMetaCursor: false,
});

runRequiresSearchMetaCursorTest({
    pipeline: [{$search: mongotQuery}, {$limit: 1}, {$project: {meta: "$$SEARCH_META"}}],
    coll: shardedColl,
    expectedDocs: [{_id: 11, meta: {val: 1}}],
    shouldRequireSearchMetaCursor: true
});

runRequiresSearchMetaCursorTest({
    pipeline: [{$search: mongotQuery}, {$sort: {_id: -1}}, {$limit: 2}],
    coll: shardedColl,
    expectedDocs: [{_id: 14, x: "cow", y: "lorem ipsum"}, {_id: 13, x: "brown", y: "ipsum"}],
    shouldRequireSearchMetaCursor: false
});

runRequiresSearchMetaCursorTest({
    pipeline:
        [{$search: mongotQuery}, {$sort: {_id: -1}}, {$project: {_id: 0, foo: "$$SEARCH_META"}}],
    coll: shardedColl,
    expectedDocs: [
        {foo: {val: 1}},
        {foo: {val: 1}},
        {foo: {val: 1}},
        {foo: {val: 1}},
        {foo: {val: 1}},
        {foo: {val: 1}},
        {foo: {val: 1}},
        {foo: {val: 1}}
    ],
    shouldRequireSearchMetaCursor: true
});

runRequiresSearchMetaCursorTest({
    pipeline: [{$search: mongotQuery}, {$sort: {_id: -1}}, {$limit: 4}, {$project: {_id: 1}}],
    coll: shardedColl,
    expectedDocs: [{_id: 14}, {_id: 13}, {_id: 12}, {_id: 11}],
    shouldRequireSearchMetaCursor: false
});

runRequiresSearchMetaCursorTest({
    pipeline: [{$search: mongotQuery}, {$limit: 1}, {$addFields: {meta: "$$SEARCH_META.val"}}],
    coll: shardedColl,
    expectedDocs: [{_id: 11, x: "brown", y: "ipsum", meta: 1}],
    shouldRequireSearchMetaCursor: true
});

runRequiresSearchMetaCursorTest({
    pipeline: [
        {$search: mongotQuery},
        {$group: {_id: "$y", x: {$addToSet: "$x"}}},
        {$match: {_id: {$ne: null}}},
        {$sort: {_id: 1}}
    ],
    coll: shardedColl,
    expectedDocs: [
        {_id: "ipsum", x: ["brown"]},
        {_id: "lorem", x: ["now"]},
        {_id: "lorem ipsum", x: ["cow"]}
    ],
    shouldRequireSearchMetaCursor: false
});

runRequiresSearchMetaCursorTest({
    pipeline: [
        {$search: mongotQuery},
        {$group: {_id: "$x", meta: {$first: "$$SEARCH_META"}}},
        {$sort: {_id: -1}},
        {$limit: 2}
    ],
    coll: shardedColl,
    expectedDocs: [{_id: "ow", meta: {val: 1}}, {_id: "now", meta: {val: 1}}],
    shouldRequireSearchMetaCursor: true
});

runRequiresSearchMetaCursorTest({
    pipeline: [
        {$search: mongotQuery},
        {$sort: {_id: 1}},
        {$group: {_id: "$x", y: {$first: "$y"}}},
        {$sort: {_id: 1}},
        {$limit: 1},
        {$project: {_id: 1, y: 1, meta: "$$SEARCH_META"}}
    ],
    coll: shardedColl,
    expectedDocs: [{_id: "brown", y: "ipsum", meta: {val: 1}}],
    shouldRequireSearchMetaCursor: true
});

runRequiresSearchMetaCursorTest({
    pipeline: [{$searchMeta: mongotQuery}],
    coll: shardedColl,
    expectedDocs: [{val: 1}],
    shouldRequireSearchMetaCursor: true,
    hasSearchMetaStage: true
});

runRequiresSearchMetaCursorTest({
    pipeline: [{$searchMeta: mongotQuery}, {$limit: 1}, {$project: {val: 0}}],
    coll: shardedColl,
    expectedDocs: [{}],
    shouldRequireSearchMetaCursor: true,
    hasSearchMetaStage: true
});

// Run tests on the unsharded collection, with a $lookup and $unionWith that have a subpipeline on
// the sharded collection.
runRequiresSearchMetaCursorTest({
    pipeline: [
        {$match: {b: 3}},
        {
            $lookup: {
                from: shardedCollName,
                pipeline: [
                    {$search: mongotQuery},
                    {$match: {y: "ipsum"}},
                    {$project : {_id: 1, meta: "$$SEARCH_META"}},
                    {$limit: 1}
                ],
                as: "out"
            }
        },
        {$project: {_id: 0, b: 1, out: 1}}
    ],
    coll: unshardedColl,
    expectedDocs:  [{b: 3, out: [{_id: 11, meta: {val: 1}}]}],
    shouldRequireSearchMetaCursor: true,
});

runRequiresSearchMetaCursorTest({
    pipeline: [
        {$match: {b: {$gt: 3}}},
        {$project: {_id: 0}},
        {
            $unionWith: {
                coll: shardedCollName,
                pipeline: [
                    {$search: mongotQuery},
                    {$project: {_id: 0, x: 1, meta: "$$SEARCH_META"}},
                    {$limit: 1}
                ]
            }
        }
    ],
    coll: unshardedColl,
    expectedDocs: [{b: 5}, {x: "brown", meta: {val: 1}}],
    shouldRequireSearchMetaCursor: true,
});

// When $search is in a subpipeline it will always create a meta cursor, even when unnecessary.
// TODO SERVER-87079 Configure queries where $search is in a subpipeline to not create unnecessary
// meta cursor.
runRequiresSearchMetaCursorTest({
    pipeline: [
        {$match: {b: {$gt: 3}}},
        {$project: {_id: 0}},
        {
            $unionWith: {
                coll: shardedCollName,
                pipeline: [{$search: mongotQuery}, {$project: {_id: 0, x: 1}}, {$limit: 3}]
            }
        }
    ],
    coll: unshardedColl,
    expectedDocs: [{b: 5}, {x: "brown"}, {x: "brown"}, {x: "brown"}],
    shouldRequireSearchMetaCursor: true,
});

runRequiresSearchMetaCursorTest({
    pipeline: [
        {$match: {b: {$gt: 3}}},
        {$project: {_id: 0}},
        {
            $lookup: {
                from: shardedCollName,
                pipeline: [{$search: mongotQuery}, {$project: {_id: 0, x: 1}}, {$limit: 1}],
                as: "out"
            }
        }
    ],
    coll: unshardedColl,
    expectedDocs:  [{b: 5, out: [{x: "brown"}]}],
    shouldRequireSearchMetaCursor: true,
});

stWithMock.stop();
