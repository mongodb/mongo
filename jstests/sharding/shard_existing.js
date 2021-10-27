/*
 * @tags: [
 *     requires_fcv_42, # autoSplitVector not present in older v4.0 binaries
 * ]
 */

(function() {
'use strict';

var s = new ShardingTest({name: "shard_existing", shards: 2, mongos: 1, other: {chunkSize: 1}});
var db = s.getDB("test");

var stringSize = 10000;
var numDocs = 2000;

// we want a lot of data, so lets make a string to cheat :)
var bigString = new Array(stringSize).toString();
var docSize = Object.bsonsize({_id: numDocs, s: bigString});
var totalSize = docSize * numDocs;
print("NumDocs: " + numDocs + " DocSize: " + docSize + " TotalSize: " + totalSize);

var bulk = db.data.initializeUnorderedBulkOp();
for (var i = 0; i < numDocs; i++) {
    bulk.insert({_id: i, s: bigString});
}
assert.writeOK(bulk.execute());

var avgObjSize = db.data.stats().avgObjSize;
var dataSize = db.data.stats().size;
assert.lte(totalSize, dataSize);

s.adminCommand({enablesharding: "test"});
s.ensurePrimaryShard('test', s.shard1.shardName);
var res = s.adminCommand({shardcollection: "test.data", key: {_id: 1}});
printjson(res);

// number of chunks should be approx equal to the total data size / chunk size
var numChunks = s.config.chunks.find({ns: 'test.data'}).itcount();
var guess = Math.ceil(dataSize / (1024 * 1024 + avgObjSize));
assert.lte(Math.abs(numChunks - guess), 2, "not right number of chunks");

s.stop();
})();
