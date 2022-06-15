/**
 * Tests that a change stream on a sharded collection with a non-simple default collation is not
 * erroneously invalidated upon chunk migration. Reproduction script for the bug in SERVER-33944.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
load("jstests/libs/change_stream_util.js");        // For 'ChangeStreamTest'.

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {
        nodes: 1,
    },
});

const testDB = st.s.getDB(jsTestName());

// Enable sharding on the test database and ensure that the primary is shard0.
assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName()}));
st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);

const caseInsensitiveCollectionName = "change_stream_case_insensitive";
const caseInsensitive = {
    locale: "en_US",
    strength: 2
};

// Create the collection with a case-insensitive collation, then shard it on {shardKey: 1}.
const caseInsensitiveCollection = assertDropAndRecreateCollection(
    testDB, caseInsensitiveCollectionName, {collation: caseInsensitive});
assert.commandWorked(
    caseInsensitiveCollection.createIndex({shardKey: 1}, {collation: {locale: "simple"}}));
assert.commandWorked(testDB.adminCommand({
    shardCollection: caseInsensitiveCollection.getFullName(),
    key: {shardKey: 1},
    collation: {locale: "simple"}
}));

// Verify that the collection does not exist on shard1.
assert(!st.shard1.getCollection(caseInsensitiveCollection.getFullName()).exists());

// Now open a change stream on the collection.
const cst = new ChangeStreamTest(testDB);
const csCursor = cst.startWatchingChanges({
    pipeline: [{$changeStream: {}}, {$project: {docId: "$documentKey.shardKey"}}],
    collection: caseInsensitiveCollection
});

// Insert some documents into the collection.
assert.commandWorked(caseInsensitiveCollection.insert({shardKey: 0, text: "aBc"}));
assert.commandWorked(caseInsensitiveCollection.insert({shardKey: 1, text: "abc"}));

// Move a chunk from shard0 to shard1. This will create the collection on shard1.
assert.commandWorked(testDB.adminCommand({
    moveChunk: caseInsensitiveCollection.getFullName(),
    find: {shardKey: 1},
    to: st.rs1.getURL(),
    _waitForDelete: false
}));

// Attempt to read from the change stream. We should see both inserts, without an invalidation.
cst.assertNextChangesEqual({cursor: csCursor, expectedChanges: [{docId: 0}, {docId: 1}]});

st.stop();
})();
