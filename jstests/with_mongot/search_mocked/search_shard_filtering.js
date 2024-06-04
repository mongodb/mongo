/**
 * Tests that the _id lookups performed by $search have a shard filter applied to them so that
 * orphans are not returned.
 */
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
const collName = "internal_search_mongot_remote";

// This test deliberately creates orphans to test shard filtering.
TestData.skipCheckOrphans = true;

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
const collNS = testColl.getFullName();

assert.commandWorked(testDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

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

// Shard the test collection, split it at {shardKey: 10}, and move the higher chunk to shard1.
assert.commandWorked(testColl.createIndex({shardKey: 1}));

// 'waitForDelete' is set to 'true' so that range deletion completes before we insert our orphan.
st.shardColl(
    testColl, {shardKey: 1}, {shardKey: 10}, {shardKey: 10 + 1}, dbName, true /* waitForDelete */);

const shard0Conn = st.rs0.getPrimary();
const shard1Conn = st.rs1.getPrimary();

// Shard0 should have exactly 4 documents; none of which get filtered out.
assert.eq(shard0Conn.getDB(dbName)[collName].find().itcount(), 4);
assert.eq(testColl.find().itcount(), 8);

// Insert a document into shard 0 which is not owned by that shard.
assert.commandWorked(shard0Conn.getDB(dbName)[collName].insert(
    {_id: 15, shardKey: 100, x: "_should be filtered out"}));

// Verify that the orphaned document exists on shard0, but that it gets filtered out when
// querying 'testColl'.
assert.eq(shard0Conn.getDB(dbName)[collName].find({_id: 15}).itcount(), 1);
assert.eq(testColl.find().itcount(), 8);

// Insert a document into shard 0 which doesn't have a shard key. This document should not be
// skipped when mongot returns a result indicating that it matched the text query. The server
// should not crash and the operation should not fail.
assert.commandWorked(shard0Conn.getDB(dbName)[collName].insert({_id: 16}));

const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), collName);

const mongotQuery = {};
const responseOk = 1;

const mongot0ResponseBatch = [
    // Mongot will act "stale": it will have an index entry for a document not owned by shard
    // 0.
    {_id: 15, $searchScore: 102},

    // The document with _id 16 has no shard key. (Perhaps it was inserted manually). This should
    // not be filtered out, because documents with missing shard key values will be placed on the
    // chunk that they would be placed at if there were null values for the shard key fields.
    {_id: 16, $searchScore: 101},

    // The remaining documents rightfully belong to shard 0.
    {_id: 3, $searchScore: 100},
    {_id: 2, $searchScore: 10},
    {_id: 4, $searchScore: 1},
    {_id: 1, $searchScore: 0.99},
];
const expectedMongotCommand = mongotCommandForQuery({
    query: mongotQuery,
    collName: collName,
    db: dbName,
    collectionUUID: collUUID0,
    protocolVersion: NumberInt(1)
});

const history0 = [{
    expectedCommand: expectedMongotCommand,
    response: mongotMultiCursorResponseForBatch(
        mongot0ResponseBatch, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk)
}];
const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
s0Mongot.setMockResponses(history0, NumberLong(123), NumberLong(1124));

const mongot1ResponseBatch = [
    {_id: 11, $searchScore: 111},
    {_id: 13, $searchScore: 30},
    {_id: 12, $searchScore: 29},
    {_id: 14, $searchScore: 28},
];
const history1 = [{
    expectedCommand: expectedMongotCommand,
    response: mongotMultiCursorResponseForBatch(
        mongot1ResponseBatch, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk)
}];
const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
s1Mongot.setMockResponses(history1, NumberLong(456), NumberLong(1457));

mockPlanShardedSearchResponse(collName, mongotQuery, dbName, undefined /*sortSpec*/, stWithMock);

const expectedDocs = [
    {_id: 11, shardKey: 100, x: "brown", y: "ipsum"},
    {_id: 16},
    {_id: 3, shardKey: 0, x: "brown", y: "ipsum"},
    {_id: 13, shardKey: 100, x: "brown", y: "ipsum"},
    {_id: 12, shardKey: 100, x: "cow", y: "lorem ipsum"},
    {_id: 14, shardKey: 100, x: "cow", y: "lorem ipsum"},
    {_id: 2, shardKey: 0, x: "now", y: "lorem"},
    {_id: 4, shardKey: 0, x: "cow", y: "lorem ipsum"},
    {_id: 1, shardKey: 0, x: "ow"},
];

assert.eq(testColl.aggregate([{$search: mongotQuery}]).toArray(), expectedDocs);

// Verify that our orphaned document is still on shard0.
assert.eq(shard0Conn.getDB(dbName)[collName].find({_id: 15}).itcount(), 1);

stWithMock.stop();
