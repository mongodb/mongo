/**
 * Verify that `$vectorSearch` queries that set 'vectorSearchScore' succeed on unsharded
 * collections on sharded clusters.
 * @tags: [
 *   requires_fcv_71,
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {mongotCommandForVectorSearchQuery} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";
import {prepCollection} from "jstests/with_mongot/mongotmock/lib/utils.js";

const dbName = jsTestName();
const collName = jsTestName();
const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_vector_search",
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
const testColl = testDB.getCollection(collName);

// Ensure db's primary shard is shard1 so we only set the correct mongot to have history.
assert.commandWorked(testDB.adminCommand({enableSharding: dbName, primaryShard: st.shard1.name}));
prepCollection(mongos, dbName, collName);

const shard1Conn = st.rs1.getPrimary();
const collUUID = getUUIDFromListCollections(testDB, testColl.getName());
const vectorSearchQuery = {
    queryVector: [1.0, 2.0, 3.0],
    path: "x",
    numCandidates: 10,
    limit: 5
};
const vectorSearchCmd = mongotCommandForVectorSearchQuery({
    ...vectorSearchQuery,
    collName: testColl.getName(),
    dbName: testDB.getName(),
    collectionUUID: collUUID,
});
{
    const shard1History = [
        {
            expectedCommand: vectorSearchCmd,
            response: {
                ok: 1,
                cursor: {
                    id: NumberLong(0),
                    ns: testColl.getFullName(),
                    nextBatch: [
                        {_id: 2, $vectorSearchScore: 0.654},
                        {_id: 1, $vectorSearchScore: 0.321},
                        {_id: 11, $vectorSearchScore: .2},
                        {_id: 12, $vectorSearchScore: .5}
                    ]
                },
            }
        },
    ];

    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(shard1History, NumberLong(123));
}

let cursor = testColl.aggregate(
    [
        {$vectorSearch: vectorSearchQuery},
        {$project: {_id: 1, x: 1, score: {$meta: "vectorSearchScore"}}}
    ],
    {cursor: {}});

const expected = [
    {"_id": 2, x: "now", score: 0.654},
    {"_id": 1, x: "ow", score: 0.321},
    {"_id": 11, x: "brown", score: .2},
    {"_id": 12, x: "cow", score: .5}
];
assert.eq(expected, cursor.toArray());
stWithMock.stop();
