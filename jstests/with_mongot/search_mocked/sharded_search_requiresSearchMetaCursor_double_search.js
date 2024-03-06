/**
 * Checks that the requiresSearchMetaCursor is set properly on search requests dispatched from
 * to mongod by running search queries and checking the shards' system.profile collection.
 *
 * This file specifically looks at queries that have two $search stages, where one is in a
 * sub-pipeline.
 *
 * Note that you can't access $$SEARCH_META after a stage with a sub-pipeline, so the top-level
 * pipeline can only reference $$SEARCH_META prior to a $lookup or $unionWith.
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

let mongos = st.s;
let testDB = mongos.getDB(dbName);
assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const shardedColl = testDB.getCollection(shardedCollName);
shardedColl.drop();
const shardedCollNS = shardedColl.getFullName();
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

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
st.shardColl(shardedColl, {_id: 1}, {_id: 10}, {_id: 10 + 1});

const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), shardedCollName);
const mongotQuery = {};
const protocolVersion = NumberInt(1);
const expectedMongotCommand =
    mongotCommandForQuery(mongotQuery, shardedCollName, dbName, collUUID0, protocolVersion);

let cursorId = NumberLong(123);
let metaCursorId = NumberLong(cursorId + 1001);
const responseOk = 1;

let shard0Conn = st.rs0.getPrimary();
let shard0DB = shard0Conn.getDB(dbName);
let shard1Conn = st.rs1.getPrimary();
let shard1DB = shard1Conn.getDB(dbName);

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

    cursorId++;
    metaCursorId++;
}

function mockPlanShardedSearchFromMongos() {
    mockPlanShardedSearchResponse(
        shardedColl.getName(), mongotQuery, dbName, undefined /*sortSpec*/, stWithMock);
}

// Mongos will non-deterministically choose a shard to run the sub-pipeline, so we'll mock a
// planShardedSearch request with {maybeUnused: true} for both shards, knowing only one will be
// executed.
function mockPlanShardedSearchFromShards() {
    mockPlanShardedSearchResponseOnConn(shardedColl.getName(),
                                        mongotQuery,
                                        dbName,
                                        undefined /*sortSpec*/,
                                        stWithMock,
                                        shard0Conn,
                                        /*maybeUnused*/ true);
    mockPlanShardedSearchResponseOnConn(shardedColl.getName(),
                                        mongotQuery,
                                        dbName,
                                        undefined /*sortSpec*/,
                                        stWithMock,
                                        shard1Conn,
                                        /*maybeUnused*/ true);
}

function resetShardProfilers() {
    for (let shardDB of [shard0DB, shard1DB]) {
        shardDB.setProfilingLevel(0);
        shardDB.system.profile.drop();
        shardDB.setProfilingLevel(2);
    }
}

/**
 * Sets up the mock responses, runs the given pipeline, then confirms via system.profile that
 * the requiresSearchMetaCursor field is set properly when commands are dispatched to shards.
 */
function runRequiresSearchMetaCursorTest({
    pipeline,
    subPipelinePlanShardedSearchDispatchedToShards,
    expectedDocs,
    outerPipelineShouldRequireSearchMetaCursor,
    innerPipelineShouldRequireSearchMetaCursor
}) {
    // Each pipeline will run planShardedSearch from mongos for the top-level $search. The
    // subPipelinePlanShardedSearchDispatchedToShards arguments indicates if the sub-pipeline will
    // run planShardedSearch from mongos or from a shard. We mock the shards twice, once per
    // $search.
    mockPlanShardedSearchFromMongos();
    if (subPipelinePlanShardedSearchDispatchedToShards) {
        mockPlanShardedSearchFromShards();
    } else {
        mockPlanShardedSearchFromMongos();
    }
    mockShards();
    mockShards();

    // We disable the order check since the order of execution is non-deterministic.
    const shard0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    shard0Mongot.disableOrderCheck();
    const shard1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    shard1Mongot.disableOrderCheck();

    resetShardProfilers();

    const comment = "search_query";
    // Run the query and ensure metrics are as expected.
    assert.eq(shardedColl.aggregate(pipeline, {comment}).toArray(), expectedDocs);

    for (let shardDB of [shard0DB, shard1DB]) {
        // We expect each shard to have two entries, one per $search. One shard may also have an
        // entry for $mergeCursors, which is filtered out by the last predicate.
        const res = shardDB.system.profile
                        .find({
                            "command.comment": comment,
                            "command.aggregate": shardedCollName,
                            "command.pipeline.0.$search": {$exists: true}
                        })
                        .toArray();
        assert.eq(2, res.length, res);

        assert.eq(outerPipelineShouldRequireSearchMetaCursor,
                  res[0].command.pipeline[0].$search.requiresSearchMetaCursor,
                  res);
        assert.eq(innerPipelineShouldRequireSearchMetaCursor,
                  res[1].command.pipeline[0].$search.requiresSearchMetaCursor,
                  res);
    }
}

// Run a pipeline with no references to $$SEARCH_META.
runRequiresSearchMetaCursorTest({
    pipeline: [
        {$search: mongotQuery},
        {$sort: {_id: 1}},
        {$limit: 1},
        {
            $unionWith: {
                coll: shardedCollName,
                pipeline: [{$search: mongotQuery}, {$sort: {_id: -1}}, {$limit: 1}]
            }
        }
    ],
    expectedDocs: [{_id: 1, x: "ow"}, {_id: 14, x: "cow", y: "lorem ipsum"}],
    subPipelinePlanShardedSearchDispatchedToShards: true,
    outerPipelineShouldRequireSearchMetaCursor: false,
    // TODO SERVER-87079 Fix queries where $search is in a subpipeline to not create unnecessary
    // meta cursor.
    innerPipelineShouldRequireSearchMetaCursor: true
});

// Run a pipeline with references to $$SEARCH_META in the top-level and sub-pipelines.
runRequiresSearchMetaCursorTest({
    pipeline: [
        {$search: mongotQuery},
        {$sort: {_id: 1}},
        {$limit: 1},
        {$project: {meta: "$$SEARCH_META"}},
        {
            $unionWith: {
                coll: shardedCollName,
                pipeline: [
                    {$search: mongotQuery},
                    {$sort: {_id: -1}},
                    {$limit: 1},
                    {$project: {meta: "$$SEARCH_META"}}
                ]
            }
        }
    ],
    expectedDocs: [{_id: 1, meta: {val: 1}}, {_id: 14, meta: {val: 1}}],
    subPipelinePlanShardedSearchDispatchedToShards: true,
    outerPipelineShouldRequireSearchMetaCursor: true,
    innerPipelineShouldRequireSearchMetaCursor: true
});

// Run a pipeline with a reference to $$SEARCH_META only in the sub-pipeline.
runRequiresSearchMetaCursorTest({
    pipeline: [
        {$search: mongotQuery},
        {$match: {_id: {$lt: 3}}},
        {
            $lookup: {
                from: shardedCollName,
                pipeline: [
                    {$search: mongotQuery},
                    {$match: {y: "ipsum"}},
                    {$project : {_id: 0, x: 1, meta: "$$SEARCH_META"}},
                    {$limit: 1}
                ],
                as: "out"
            }
        },
        {$project: {_id: 1, out: 1}}
    ],
    expectedDocs: [{_id: 2, out: [{x: "brown", meta: {val: 1}}]}, {_id: 1, out: [{x: "brown", meta: {val: 1}}]}],
    // This usage of $lookup after $search will dispatch both planShardedSearch requests from mongos.
    subPipelinePlanShardedSearchDispatchedToShards: false,
    outerPipelineShouldRequireSearchMetaCursor: false,
    innerPipelineShouldRequireSearchMetaCursor: true
});

// Run a pipeline with a reference to $$SEARCH_META only in the top-level pipeline.
runRequiresSearchMetaCursorTest({
    pipeline: [
        {$search: mongotQuery},
        {$limit: 2},
        {$project: {_id: 0, b: 1, meta: "$$SEARCH_META.val"}},
        {
            $lookup: {
                from: shardedCollName,
                pipeline: [
                    {$search: mongotQuery},
                    {$sort: {y: -1}},
                    {$limit: 1},
                    {$project: {_id: 0}}
                ],
                as: "out"
            }
        },
    ],
    expectedDocs: [
        {meta: 1, out: [{x: "cow", y: "lorem ipsum"}]},
        {meta: 1, out: [{x: "cow", y: "lorem ipsum"}]}
    ],
    // This usage of $lookup after $search will dispatch both planShardedSearch requests from mongos.
    subPipelinePlanShardedSearchDispatchedToShards: false,
    outerPipelineShouldRequireSearchMetaCursor: true,
    // TODO SERVER-87079 Fix queries where $search is in a subpipeline to not create unnecessary
    // meta cursor.
    innerPipelineShouldRequireSearchMetaCursor: true
});

stWithMock.stop();
