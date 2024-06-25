/**
 * Verify that a `$search` query containing a `$unionWith` that sets
 * '$$SEARCH_META' succeeds on sharded collections.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    getDefaultProtocolVersionForPlanShardedSearch,
    mongotCommandForQuery
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = jsTestName();
const collName = jsTestName();
const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
    shards: {
        rs0: {nodes: 1},
        rs1: {nodes: 1},
    },
    mongos: 1,
});
stWithMock.start();
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
const protocolVersion = getDefaultProtocolVersionForPlanShardedSearch();
assert.commandWorked(testDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const testColl = testDB.getCollection(collName);

// Documents that end up on shard0.
assert.commandWorked(testColl.insert({
    _id: 1,
    shardKey: 0,
}));
assert.commandWorked(testColl.insert({
    _id: 2,
    shardKey: 0,
}));
// Documents that end up on shard1.
assert.commandWorked(testColl.insert({
    _id: 11,
    shardKey: 100,
}));
assert.commandWorked(testColl.insert({
    _id: 12,
    shardKey: 100,
}));

// Shard the test collection, split it at {shardKey: 10}, and move the higher chunk to shard1.
assert.commandWorked(testColl.createIndex({shardKey: 1}));
st.shardColl(testColl, {shardKey: 1}, {shardKey: 10}, {shardKey: 10 + 1});

const mongotQuery = {};
function mockPlanShardedSearchResponseLocal(conn, cursorId = 1, maybeUnused = false) {
    const mergingPipelineHistory = [{
        expectedCommand: {
            planShardedSearch: testColl.getName(),
            query: mongotQuery,
            $db: dbName,
            searchFeatures: {shardedSort: 1}
        },
        response: {
            ok: 1,
            protocolVersion: protocolVersion,
            // This does not represent an actual merging pipeline. The merging pipeline is
            // arbitrary, it just must only generate one document.
            metaPipeline: [
                {
                    "$group": {
                        "_id": {
                            "type": "$type",
                        },
                        "sum": {
                            "$sum": "$count",
                        }
                    }
                },
                {$project: {_id: 0, type: "$_id.type", count: "$sum"}},
                {$match: {"type": 1}}
            ]
        },
        maybeUnused: maybeUnused,
    }];
    const mongot = stWithMock.getMockConnectedToHost(conn);
    mongot.setMockResponses(mergingPipelineHistory, cursorId);
}

const shard0Conn = st.rs0.getPrimary();
const shard1Conn = st.rs1.getPrimary();
const collUUID0 = getUUIDFromListCollections(shard0Conn.getDB(dbName), collName);
const collUUID1 = getUUIDFromListCollections(shard1Conn.getDB(dbName), collName);

// Mock PSS for initial $search parsing in mongos.
mockPlanShardedSearchResponseLocal(mongos, 1, false);

{
    // Main pipeline response.
    const shard0MainHistory = [
        {
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: testColl.getName(),
                db: testDB.getName(),
                collectionUUID: collUUID1,
                protocolVersion: protocolVersion
            }),
            response: {
                ok: 1,
                cursors: [
                    {
                        cursor: {
                            id: NumberLong(0),
                            ns: testColl.getFullName(),
                            nextBatch: [
                                {_id: 1, $searchScore: 0.321},
                                {_id: 2, $searchScore: 0.321},
                            ],
                            type: "results",
                        },
                        ok: 1
                    },
                    {
                        cursor: {
                            id: NumberLong(0),
                            ns: testColl.getFullName(),
                            nextBatch: [{type: 1, count: 15}],
                            type: "meta",
                        },
                        ok: 1
                    }
                ]
            }
        },
    ];

    const shard0UnionHistory = [
        {
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: testColl.getName(),
                db: testDB.getName(),
                collectionUUID: collUUID1,
                protocolVersion: protocolVersion
            }),
            response: {
                ok: 1,
                cursors: [
                    {
                        cursor: {
                            id: NumberLong(0),
                            ns: testColl.getFullName(),
                            nextBatch: [
                                {_id: 1, $searchScore: 0.321},
                                {_id: 2, $searchScore: 0.321},
                            ],
                            type: "results",
                        },
                        ok: 1
                    },
                    {
                        cursor: {
                            id: NumberLong(0),
                            ns: testColl.getFullName(),
                            nextBatch: [{type: 1, count: 105}],
                            type: "meta",
                        },
                        ok: 1
                    }
                ]
            }
        },
    ];

    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(shard0MainHistory, NumberLong(123), NumberLong(125));
    // Mock PSS for $unionWith parsing in mongod shard.
    // The $unionWith is dispatched to either shard randomly, we can't predicate the PSS is issued
    // in which shard so we need to set it as 'MayUnused'.
    mockPlanShardedSearchResponseLocal(shard0Conn, 7, true);
    s0Mongot.setMockResponses(shard0UnionHistory, NumberLong(124), NumberLong(126));
    // If the PSS is not issued in this shard, following search mock will fail due to order check.
    s0Mongot.disableOrderCheck();
}

{
    // Main pipeline response.
    const shard1MainHistory = [
        {
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: testColl.getName(),
                db: testDB.getName(),
                collectionUUID: collUUID1,
                protocolVersion: protocolVersion
            }),
            response: {
                ok: 1,
                cursors: [
                    {
                        cursor: {
                            id: NumberLong(0),
                            ns: testColl.getFullName(),
                            nextBatch: [
                                {_id: 11, $searchScore: 0.654},
                                {_id: 12, $searchScore: 0.654},
                            ],
                            type: "results",
                        },
                        ok: 1
                    },
                    {
                        cursor: {
                            id: NumberLong(0),
                            ns: testColl.getFullName(),
                            nextBatch: [{type: 1, count: 25}

                            ],
                            type: "meta",
                        },
                        ok: 1
                    },
                ]
            }
        },
    ];

    const shard1UnionHistory = [{
        expectedCommand: mongotCommandForQuery({
            query: mongotQuery,
            collName: testColl.getName(),
            db: testDB.getName(),
            collectionUUID: collUUID1,
            protocolVersion: protocolVersion
        }),
        response: {
            ok: 1,
            cursors: [
                {
                    cursor: {
                        id: NumberLong(0),
                        ns: testColl.getFullName(),
                        nextBatch: [
                            {_id: 11, $searchScore: 0.654},
                            {_id: 12, $searchScore: 0.654},
                        ],
                        type: "results",
                    },
                    ok: 1
                },
                {
                    cursor: {
                        id: NumberLong(0),
                        ns: testColl.getFullName(),
                        nextBatch: [{type: 1, count: 205}],
                        type: "meta",
                    },
                    ok: 1
                },
            ]
        }
    }];

    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(shard1MainHistory, NumberLong(127), NumberLong(129));
    // See shard0 comments.
    mockPlanShardedSearchResponseLocal(shard1Conn, 8, true);
    s1Mongot.setMockResponses(shard1UnionHistory, NumberLong(128), NumberLong(130));
    s1Mongot.disableOrderCheck();
}

let result = assert.commandWorked(testDB.runCommand({
    aggregate: testColl.getName(),
    pipeline: [
        {$search: mongotQuery},
        {$project: {_id: 1, pipe: "outer", meta: "$$SEARCH_META"}},
        {
            $unionWith: {
                coll: testColl.getName(),
                pipeline: [
                    {$search: mongotQuery},
                    {$project: {_id: 1, pipe: "inner", meta: "$$SEARCH_META"}}
                ]
            }
        }
    ],
    cursor: {}
}));
const outerMetadataDoc = {
    type: 1,
    count: 40
};
const innerMetadataDoc = {
    type: 1,
    count: 310
};
const expectedResults = [
    {_id: 1, pipe: "outer", meta: outerMetadataDoc},
    {_id: 2, pipe: "outer", meta: outerMetadataDoc},
    {_id: 11, pipe: "outer", meta: outerMetadataDoc},
    {_id: 12, pipe: "outer", meta: outerMetadataDoc},
    {_id: 1, pipe: "inner", meta: innerMetadataDoc},
    {_id: 2, pipe: "inner", meta: innerMetadataDoc},
    {_id: 11, pipe: "inner", meta: innerMetadataDoc},
    {_id: 12, pipe: "inner", meta: innerMetadataDoc},
];
assert.sameMembers(result.cursor.firstBatch, expectedResults);
jsTestLog(tojson(result));
stWithMock.stop();
