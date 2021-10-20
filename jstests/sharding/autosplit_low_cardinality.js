/**
 * Test the autosplitter when a collection has very low cardinality
 */

(function() {
'use strict';
load('jstests/sharding/autosplit_include.js');
load("jstests/sharding/libs/find_chunks_util.js");

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
    return findChunksUtil.countChunksForNs(st.config, "test.foo");
}

// Accumulate ~1MB of documents under -10 and +10
insertBigDocsWithKey(-10, 4);
insertBigDocsWithKey(10, 4);
waitForOngoingChunkSplits(st);

let expectedNumChunks = 2;
try {
    // At least one split should have been performed
    assert.gte(numChunks(),
               expectedNumChunks,
               "Number of chunks is less than 2, no split have been perfomed");
} catch (e) {
    // (SERVER-59882) split may not have happened due to commit delay of the inserted documents
    print("Retrying performing one insert after catching exception " + e);
    insertBigDocsWithKey(10, 1);
    waitForOngoingChunkSplits(st);
    assert.gte(
        numChunks(),
        expectedNumChunks,
        "Number of chunks is less than " + expectedNumChunks + ", no split has been perfomed");
}

expectedNumChunks++;

insertBigDocsWithKey(20, 4);
waitForOngoingChunkSplits(st);
// An additional split should have been performed
try {
    assert.gte(numChunks(), expectedNumChunks, "Number of chunks must be at least 3");
} catch (e) {
    // (SERVER-59882) split may not have happened due to commit delay of the inserted documents
    print("Retrying performing one insert after catching exception " + e);
    insertBigDocsWithKey(20, 1);
    waitForOngoingChunkSplits(st);
    assert.gte(
        numChunks(),
        expectedNumChunks,
        "Number of chunks is less than " + 3 + ", not all expected splits have been perfomed");
}

st.stop();
})();
