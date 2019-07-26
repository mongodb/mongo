/**
 * This test confirms that chunks get split as they grow due to data insertion.
 */
(function() {
'use strict';
load('jstests/sharding/autosplit_include.js');

var s = new ShardingTest({
    name: "auto1",
    shards: 2,
    mongos: 1,
    other: {enableAutoSplit: true, chunkSize: 10},
});

assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
s.ensurePrimaryShard('test', s.shard1.shardName);
assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {num: 1}}));

var bigString = "";
while (bigString.length < 1024 * 50)
    bigString += "asocsancdnsjfnsdnfsjdhfasdfasdfasdfnsadofnsadlkfnsaldknfsad";

var db = s.getDB("test");
var primary = s.getPrimaryShard("test").getDB("test");
var coll = db.foo;
var counts = [];

var i = 0;

// Inserts numDocs documents into the collection, waits for any ongoing
// splits to finish, and then prints some information about the
// collection's chunks
function insertDocsAndWaitForSplit(numDocs) {
    var bulk = coll.initializeUnorderedBulkOp();
    var curMaxKey = i;
    // Increment the global 'i' variable to keep 'num' unique across all
    // documents
    for (; i < curMaxKey + numDocs; i++) {
        bulk.insert({num: i, s: bigString});
    }
    assert.writeOK(bulk.execute());

    waitForOngoingChunkSplits(s);

    s.printChunks();
    s.printChangeLog();
}

insertDocsAndWaitForSplit(100);

counts.push(s.config.chunks.count({"ns": "test.foo"}));
assert.eq(100, db.foo.find().itcount());

print("datasize: " +
      tojson(s.getPrimaryShard("test").getDB("admin").runCommand({datasize: "test.foo"})));

insertDocsAndWaitForSplit(100);
counts.push(s.config.chunks.count({"ns": "test.foo"}));

insertDocsAndWaitForSplit(200);
counts.push(s.config.chunks.count({"ns": "test.foo"}));

insertDocsAndWaitForSplit(300);
counts.push(s.config.chunks.count({"ns": "test.foo"}));

assert(counts[counts.length - 1] > counts[0], "counts 1 : " + tojson(counts));
var sorted = counts.slice(0);
// Sort doesn't sort numbers correctly by default, resulting in fail
sorted.sort(function(a, b) {
    return a - b;
});
assert.eq(counts, sorted, "counts 2 : " + tojson(counts));

print(counts);

printjson(db.stats());

s.stop();
})();
