/**
 * Where we test operations dealing with large chunks
 *
 * This test is labeled resource intensive because its total io_write is 220MB compared to a median
 * of 5MB across all sharding tests in wiredTiger.
 * @tags: [
 *  resource_intensive,
 * ]
 */
(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

// Starts a new sharding environment limiting the chunk size to 1GB (highest value allowed).
// Note that early splitting will start with a 1/4 of max size currently.
var st = new ShardingTest({name: 'large_chunk', shards: 2, other: {chunkSize: 1024}});
var db = st.getDB("test");

//
// Step 1 - Test moving a large chunk
//

// Turn on sharding on the 'test.foo' collection and generate a large chunk
assert.commandWorked(st.s0.adminCommand({enablesharding: "test"}));
st.ensurePrimaryShard('test', st.shard1.shardName);

var bigString = "";
while (bigString.length < 10000) {
    bigString += "asdasdasdasdadasdasdasdasdasdasdasdasda";
}

var inserted = 0;
var num = 0;
var bulk = db.foo.initializeUnorderedBulkOp();
while (inserted < (400 * 1024 * 1024)) {
    bulk.insert({_id: num++, s: bigString});
    inserted += bigString.length;
}
assert.commandWorked(bulk.execute());

assert.commandWorked(st.s0.adminCommand({shardcollection: "test.foo", key: {_id: 1}}));

assert.eq(
    1, findChunksUtil.countChunksForNs(st.config, "test.foo"), "step 1 - need one large chunk");

var primary = st.getPrimaryShard("test").getDB("test");
var secondary = st.getOther(primary).getDB("test");

// Make sure that we don't move that chunk if it goes past what we consider the maximum chunk
// size
jsTest.log("moveChunk expected to fail due to excessive size");
var maxMB = 200;

assert.commandWorked(
    st.s.adminCommand({configureCollectionBalancing: "test.foo", chunkSize: maxMB}));

assert.throws(function() {
    st.adminCommand({movechunk: "test.foo", find: {_id: 1}, to: secondary.getMongo().name});
});

// Move back to the default configuration and move the chunk
assert.commandWorked(st.s.adminCommand({
    configureCollectionBalancing: "test.foo",
    chunkSize: 0  // clears override
}));

jsTest.log("moveChunk expected to succeed");
var before = findChunksUtil.findChunksByNs(st.config, 'test.foo').toArray();
assert.commandWorked(
    st.s0.adminCommand({movechunk: "test.foo", find: {_id: 1}, to: secondary.getMongo().name}));

var after = findChunksUtil.findChunksByNs(st.config, 'test.foo').toArray();
assert.neq(before[0].shard, after[0].shard, "move chunk did not work");

st.config.changelog.find().forEach(printjson);

st.stop();
})();
