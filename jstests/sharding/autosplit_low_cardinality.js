/**
 * Test the autosplitter when a collection has very low cardinality
 *
 * @tags: [requires_fcv_44]
 */

(function() {
'use strict';
load('jstests/sharding/autosplit_include.js');

var st = new ShardingTest({
    name: "low_cardinality",
    other: {enableAutoSplit: true, chunkSize: 1},
});

assert.commandWorked(st.s.adminCommand({enablesharding: "test"}));
assert.commandWorked(st.s.adminCommand({shardcollection: "test.foo", key: {sk: 1}}));

const bigString = "X".repeat(1024 * 1024 / 4);  // 250 KB

var coll = st.getDB("test").getCollection("foo");

// Insert $numDocs documents into the collection under $key.
// Each document contains a string of 250KB
// waits for any ongoing splits to finish, and then prints some information
// about the collection's chunks
function insertBigDocsWithKey(key, numDocs) {
    var bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        bulk.insert({sk: key, sub: i, bs: bigString});
    }
    assert.commandWorked(bulk.execute());
    waitForOngoingChunkSplits(st);
}

function numChunks() {
    return st.config.chunks.count({"ns": "test.foo"});
}

// Accumulate ~1MB of documents under -10 and +10
insertBigDocsWithKey(-10, 4);
insertBigDocsWithKey(10, 4);
waitForOngoingChunkSplits(st);

// At least one split should have been performed
assert.gte(numChunks(), 2, "Number of chunks is less then 2, no split have been perfomed");

insertBigDocsWithKey(20, 4);
waitForOngoingChunkSplits(st);
// An additional split should have been performed
assert.gte(numChunks(), 3, "Number of chunks must be at least 3");

st.stop();
})();
