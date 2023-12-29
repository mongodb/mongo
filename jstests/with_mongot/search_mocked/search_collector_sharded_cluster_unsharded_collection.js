/**
 * Verify that `$search` queries that set '$$SEARCH_META' succeed on unsharded collections on
 * sharded clusters.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
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
// Ensure db's primary shard is shard1 so we only set the correct mongot to have history.
assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard1.name}));

const testColl = testDB.getCollection(collName);

assert.commandWorked(testColl.insert({_id: 1, shardKey: 0, x: "ow"}));
assert.commandWorked(testColl.insert({_id: 2, shardKey: 0, x: "now", y: "lorem"}));
assert.commandWorked(testColl.insert({_id: 11, shardKey: 100, x: "brown", y: "ipsum"}));
assert.commandWorked(testColl.insert({_id: 12, shardKey: 100, x: "cow", y: "lorem ipsum"}));

const shard1Conn = st.rs1.getPrimary();
const mongosConn = st.s;
const collUUID = getUUIDFromListCollections(testDB, testColl.getName());
const mongotQuery = {};
const searchCmd = {
    search: testColl.getName(),
    collectionUUID: collUUID,
    query: mongotQuery,
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

    const historyObj = [{
        expectedCommand: {
            planShardedSearch: testColl.getName(),
            query: mongotQuery,
            $db: dbName,
            searchFeatures: {shardedSort: 1}
        },
        response: {
            ok: 1,
            protocolVersion: NumberInt(42),
            // This test doesn't use metadata. Give a trivial pipeline.
            metaPipeline: [{$limit: 1}]
        }
    }];
    const mongosMongot = stWithMock.getMockConnectedToHost(mongosConn);
    mongosMongot.setMockResponses(historyObj, NumberLong(123));
}

let cursor = testColl.aggregate(
    [{$search: mongotQuery}, {$project: {_id: 1, meta: "$$SEARCH_META"}}], {cursor: {}});

const expected = [
    {"_id": 2, "meta": {value: 1}},
    {"_id": 1, "meta": {value: 1}},
    {"_id": 11, "meta": {value: 1}},
    {"_id": 12, "meta": {value: 1}}
];
assert.eq(expected, cursor.toArray());
stWithMock.stop();
