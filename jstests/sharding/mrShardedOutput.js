// This test runs map reduce from a sharded input collection and outputs it to a sharded
// collection. The test is done in 2 passes - the first pass runs the map reduce and
// outputs it to a non-existing collection. The second pass runs map reduce with the
// collection input twice the size of the first and outputs it to the new sharded
// collection created in the first pass.

var st = new ShardingTest({shards: 2, other: {chunkSize: 1}});

var config = st.getDB("config");
st.adminCommand({enablesharding: "test"});
st.ensurePrimaryShard("test", "shard0001");
st.adminCommand({shardcollection: "test.foo", key: {"a": 1}});

var testDB = st.getDB("test");

function map2() {
    emit(this.i, {count: 1, y: this.y});
}
function reduce2(key, values) {
    return values[0];
}

var numDocs = 0;
var numBatch = 5000;
var str = new Array(1024).join('a');

// Pre split now so we don't have to balance the chunks later.
// M/R is strange in that it chooses the output shards based on currently sharded
// collections in the database. The upshot is that we need a sharded collection on
// both shards in order to ensure M/R will output to two shards.
st.adminCommand({split: 'test.foo', middle: {a: numDocs + numBatch / 2}});
st.adminCommand({moveChunk: 'test.foo', find: {a: numDocs}, to: 'shard0000'});

// Add some more data for input so that chunks will get split further
for (var splitPoint = 0; splitPoint < numBatch; splitPoint += 400) {
    testDB.adminCommand({split: 'test.foo', middle: {a: splitPoint}});
}

var bulk = testDB.foo.initializeUnorderedBulkOp();
for (var i = 0; i < numBatch; ++i) {
    bulk.insert({a: numDocs + i, y: str, i: numDocs + i});
}
assert.writeOK(bulk.execute());

numDocs += numBatch;

// Do the MapReduce step
jsTest.log("Setup OK: count matches (" + numDocs + ") -- Starting MapReduce");
var res = testDB.foo.mapReduce(map2, reduce2, {out: {replace: "mrShardedOut", sharded: true}});
jsTest.log("MapReduce results:" + tojson(res));

var reduceOutputCount = res.counts.output;
assert.eq(numDocs,
          reduceOutputCount,
          "MapReduce FAILED: res.counts.output = " + reduceOutputCount + ", should be " + numDocs);

jsTest.log("Checking that all MapReduce output documents are in output collection");
var outColl = testDB["mrShardedOut"];
var outCollCount = outColl.find().itcount();
assert.eq(numDocs,
          outCollCount,
          "MapReduce FAILED: outColl.find().itcount() = " + outCollCount + ", should be " +
              numDocs + ": this may happen intermittently until resolution of SERVER-3627");

// Make sure it's sharded and split
var newNumChunks = config.chunks.count({ns: testDB.mrShardedOut._fullName});
assert.gt(
    newNumChunks, 1, "Sharding FAILURE: " + testDB.mrShardedOut._fullName + " has only 1 chunk");

// Check that there are no "jumbo" chunks.
var objSize = Object.bsonsize(testDB.mrShardedOut.findOne());
var docsPerChunk = 1024 * 1024 / objSize * 1.1;  // 1MB chunk size + allowance

st.printShardingStatus(true);

config.chunks.find({ns: testDB.mrShardedOut.getFullName()}).forEach(function(chunkDoc) {
    var count =
        testDB.mrShardedOut.find({_id: {$gte: chunkDoc.min._id, $lt: chunkDoc.max._id}}).itcount();
    assert.lte(count, docsPerChunk, 'Chunk has too many docs: ' + tojson(chunkDoc));
});

// Check that chunks for the newly created sharded output collection are well distributed.
var shard0Chunks =
    config.chunks.find({ns: testDB.mrShardedOut._fullName, shard: 'shard0000'}).count();
var shard1Chunks =
    config.chunks.find({ns: testDB.mrShardedOut._fullName, shard: 'shard0001'}).count();
assert.lte(Math.abs(shard0Chunks - shard1Chunks), 1);

jsTest.log('Starting second pass');

st.adminCommand({split: 'test.foo', middle: {a: numDocs + numBatch / 2}});
st.adminCommand({moveChunk: 'test.foo', find: {a: numDocs}, to: 'shard0000'});

// Add some more data for input so that chunks will get split further
for (splitPoint = 0; splitPoint < numBatch; splitPoint += 400) {
    testDB.adminCommand({split: 'test.foo', middle: {a: numDocs + splitPoint}});
}

bulk = testDB.foo.initializeUnorderedBulkOp();
for (var i = 0; i < numBatch; ++i) {
    bulk.insert({a: numDocs + i, y: str, i: numDocs + i});
}
assert.writeOK(bulk.execute());
jsTest.log("No errors on insert batch.");
numDocs += numBatch;

// Do the MapReduce step
jsTest.log("Setup OK: count matches (" + numDocs + ") -- Starting MapReduce");
res = testDB.foo.mapReduce(map2, reduce2, {out: {replace: "mrShardedOut", sharded: true}});
jsTest.log("MapReduce results:" + tojson(res));

reduceOutputCount = res.counts.output;
assert.eq(numDocs,
          reduceOutputCount,
          "MapReduce FAILED: res.counts.output = " + reduceOutputCount + ", should be " + numDocs);

jsTest.log("Checking that all MapReduce output documents are in output collection");
outColl = testDB["mrShardedOut"];
outCollCount = outColl.find().itcount();
assert.eq(numDocs,
          outCollCount,
          "MapReduce FAILED: outColl.find().itcount() = " + outCollCount + ", should be " +
              numDocs + ": this may happen intermittently until resolution of SERVER-3627");

// Make sure it's sharded and split
newNumChunks = config.chunks.count({ns: testDB.mrShardedOut._fullName});
assert.gt(
    newNumChunks, 1, "Sharding FAILURE: " + testDB.mrShardedOut._fullName + " has only 1 chunk");

st.printShardingStatus(true);

// TODO: fix SERVER-12581
/*
config.chunks.find({ ns: testDB.mrShardedOut.getFullName() }).forEach(function(chunkDoc) {
    var count = testDB.mrShardedOut.find({ _id: { $gte: chunkDoc.min._id,
                                                  $lt: chunkDoc.max._id }}).itcount();
    assert.lte(count, docsPerChunk, 'Chunk has too many docs: ' + tojson(chunkDoc));
});
*/

// Note: No need to check if chunk is balanced. It is the job of the balancer
// to balance chunks.

st.stop();
