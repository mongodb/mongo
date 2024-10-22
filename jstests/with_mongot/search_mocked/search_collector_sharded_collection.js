/**
 * Verify that `$search` queries that set '$$SEARCH_META' succeed on sharded collections.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {mongotCommandForQuery} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = jsTestName();
const collName = jsTestName();
let nodeOptions = {setParameter: {enableTestCommands: 1}};

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
    shards: {
        rs0: {nodes: 1},
        rs1: {nodes: 1},
    },
    mongos: 1,
    other: {
        rsOptions: nodeOptions,
        mongosOptions: nodeOptions,
    }
});
stWithMock.start();
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
assert.commandWorked(testDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
const testColl = testDB.getCollection(collName);
const shardedCollBase = testDB.getCollection("baseSharded");

// Documents that end up on shard0.
assert.commandWorked(testColl.insert({_id: 1, shardKey: 0, x: "ow"}));
assert.commandWorked(testColl.insert({_id: 2, shardKey: 0, x: "now", y: "lorem"}));
assert.commandWorked(testColl.insert({_id: 3, shardKey: 0, x: "brown", y: "ipsum"}));
assert.commandWorked(testColl.insert({_id: 4, shardKey: 0, x: "cow", y: "lorem ipsum"}));
// Documents that end up on shard1.
assert.commandWorked(testColl.insert({_id: 11, shardKey: 100, x: "brown", y: "ipsum"}));
assert.commandWorked(testColl.insert({_id: 12, shardKey: 100, x: "cow", y: "lorem ipsum"}));
assert.commandWorked(testColl.insert({_id: 13, shardKey: 100, x: "brown", y: "ipsum"}));
assert.commandWorked(testColl.insert({_id: 14, shardKey: 100, x: "cow", y: "lorem ipsum"}));

// non-search collection
assert.commandWorked(shardedCollBase.insert({"_id": 0, "x": "x1"}));
assert.commandWorked(shardedCollBase.insert({"_id": 101, "x": "x2"}));

// Shard the test collection, split it at {shardKey: 10}, and move the higher chunk to shard1.
assert.commandWorked(testColl.createIndex({shardKey: 1}));
st.shardColl(testColl, {shardKey: 1}, {shardKey: 10}, {shardKey: 10 + 1});
st.shardColl(shardedCollBase, {_id: 1}, {_id: 100}, {_id: 101});

const shard0Conn = st.rs0.getPrimary();
const shard1Conn = st.rs1.getPrimary();
const collUUID0 = getUUIDFromListCollections(shard0Conn.getDB(dbName), collName);
const collUUID1 = getUUIDFromListCollections(shard1Conn.getDB(dbName), collName);
const collUUID = collUUID0;
const collNS = testColl.getFullName();

const searchQuery = {
    query: "cakes",
    path: "title"
};

const expectedCommand = mongotCommandForQuery({
    query: searchQuery,
    collName: testColl.getName(),
    db: testDB.getName(),
    collectionUUID: collUUID,
    protocolVersion: NumberInt(42)
});

// History for shard 1.
{
    const resultsID = NumberLong(11);
    const metaID = NumberLong(12);
    const historyResults = [
        {
            expectedCommand: expectedCommand,
            response: {
                ok: 1,
                cursors: [
                    {
                        cursor: {
                            id: NumberLong(0),
                            type: "results",
                            ns: collNS,
                            nextBatch: [
                                {_id: 1, val: 1, $searchScore: .41},
                                {_id: 2, val: 2, $searchScore: .31},
                                {_id: 3, val: 3, $searchScore: .28},
                                {_id: 4, val: 4, $searchScore: .11},
                            ],
                        },
                        ok: 1
                    },
                    {
                        cursor: {
                            id: NumberLong(0),
                            ns: collNS,
                            type: "meta",
                            nextBatch: [{type: 1, count: 2}, {type: 2, count: 17}],
                        },
                        ok: 1
                    }
                ]
            }
        },
    ];
    const mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());
    mongot.setMockResponses(historyResults, resultsID, metaID);
}

// History for shard 2.
{
    const resultsID = NumberLong(21);
    const metaID = NumberLong(22);
    const historyResults = [
        {
            expectedCommand: expectedCommand,
            response: {
                ok: 1,
                cursors: [
                    {
                        cursor: {
                            id: NumberLong(0),
                            type: "results",
                            ns: collNS,
                            nextBatch: [
                                {_id: 11, val: 11, $searchScore: .4},
                                {_id: 12, val: 12, $searchScore: .3},
                                {_id: 13, val: 13, $searchScore: .2},
                                {_id: 14, val: 14, $searchScore: .1},
                            ],
                        },
                        ok: 1
                    },
                    {
                        cursor: {
                            id: NumberLong(0),
                            ns: collNS,
                            type: "meta",
                            nextBatch: [{type: 1, count: 5}, {type: 2, count: 10}],
                        },
                        ok: 1
                    }
                ]
            }
        },
    ];
    const mongot = stWithMock.getMockConnectedToHost(st.rs1.getPrimary());
    mongot.setMockResponses(historyResults, resultsID, metaID);
}

// History for mongos
{
    const mergingPipelineHistory = [{
        expectedCommand: {
            planShardedSearch: collName,
            query: searchQuery,
            $db: dbName,
            searchFeatures: {shardedSort: 1}
        },
        response: {
            ok: 1,
            protocolVersion: NumberInt(42),
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
        }
    }];
    const mongot = stWithMock.getMockConnectedToHost(stWithMock.st.s);
    mongot.setMockResponses(mergingPipelineHistory, 1);
}

let cursor = testColl.aggregate([
    {$search: searchQuery},
    {$project: {_id: 1, meta: "$$SEARCH_META"}},
]);

const metaDoc = {
    type: 1,
    count: 7
};
const expected = [
    {"_id": 1, "meta": metaDoc},
    {"_id": 11, "meta": metaDoc},
    {"_id": 2, "meta": metaDoc},
    {"_id": 12, "meta": metaDoc},
    {"_id": 3, "meta": metaDoc},
    {"_id": 13, "meta": metaDoc},
    {"_id": 4, "meta": metaDoc},
    {"_id": 14, "meta": metaDoc}
];

assert.eq(expected, cursor.toArray());

stWithMock.stop();
