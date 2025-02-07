/**
 * Verify that `$search` queries that set '$$SEARCH_META' succeed on unsharded collections on
 * sharded clusters even with a stage in the pipeline that can't be passed to the shards.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {mockPlanShardedSearchResponse} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = jsTestName();
const collName = jsTestName();
const foreignCollName = jsTestName() + ".other";
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
const testColl = testDB.getCollection(collName);
const foreignColl = testDB.getCollection(foreignCollName);

// Ensure primary shard is shard1 so we only set the correct mongot to have history.
assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard1.name}));

assert.commandWorked(testColl.insert({_id: 1, shardKey: 0, x: "ow"}));
assert.commandWorked(testColl.insert({_id: 2, shardKey: 0, x: "now", y: "lorem"}));
assert.commandWorked(testColl.insert({_id: 11, shardKey: 100, x: "brown", y: "ipsum"}));
assert.commandWorked(testColl.insert({_id: 12, shardKey: 100, x: "cow", y: "lorem ipsum"}));

const shard0Conn = st.rs0.getPrimary();
const shard1Conn = st.rs1.getPrimary();
const collUUID = getUUIDFromListCollections(testDB, testColl.getName());
const searchQuery = {};
const searchCmd = {
    search: testColl.getName(),
    collectionUUID: collUUID,
    query: searchQuery,
    $db: testDB.getName()
};
{
    const shard1History = [
        {
            expectedCommand: searchCmd,
            response: {
                ok: 1,
                cursor: {
                    id: NumberLong(0),
                    ns: testColl.getFullName(),
                    nextBatch: [
                        {_id: 2, $searchScore: 0.654},
                        {_id: 1, $searchScore: 0.321},
                        {_id: 11, $searchScore: .2},
                        {_id: 12, $searchScore: .5}
                    ]
                },
                vars: {SEARCH_META: {value: 1}}
            }
        },
    ];

    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(shard1History, NumberLong(123));
}

mockPlanShardedSearchResponse(collName, searchQuery, dbName, undefined /*sortSpec*/, stWithMock);

let cursor = testColl.aggregate(
    [
        {$search: searchQuery},
        {$project: {_id: 1, meta: "$$SEARCH_META"}},
        {$out: foreignColl.getName()}
    ],
    {cursor: {}});

assert.eq([], cursor.toArray());

let foreignArray = foreignColl.find().sort({_id: 1}).toArray();
const expected = [
    {"_id": 1, "meta": {value: 1}},
    {"_id": 2, "meta": {value: 1}},
    {"_id": 11, "meta": {value: 1}},
    {"_id": 12, "meta": {value: 1}}
];

assert.eq(expected, foreignArray);

stWithMock.stop();
