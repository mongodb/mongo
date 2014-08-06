var st = new ShardingTest({ shards: 2, chunksize: 1 });

var testDB = st.s.getDB('test');
testDB.adminCommand({ enableSharding: 'test' });
testDB.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});

testDB.adminCommand({ split: 'test.user', middle: { x: 0 }});
testDB.adminCommand({ moveChunk: 'test.user', find: { x: 0 }, to: 'shard0000' });

for (var x = -100; x < 100; x+= 10) {
    testDB.adminCommand({ split: 'test.user', middle: { x: x }});
}

st.startBalancer();

var configDB = st.s.getDB('config');
var topChunkBefore = configDB.chunks.find({}).sort({ min: -1 }).next();

// This bulk insert should only trigger a single chunk move.
var largeStr = new Array(1024).join('x');
var bulk = testDB.user.initializeUnorderedBulkOp();
for (var x = 100; x < 1000; x++) {
    bulk.insert({ x: x, val: largeStr });
}
bulk.execute();

var topChunkAfter = configDB.chunks.find({}).sort({ min: -1 }).next();

assert.neq(topChunkBefore.shard, topChunkAfter.shard,
           'chunk did not move. Before: ' + tojson(topChunkBefore) +
           ', after: ' + tojson(topChunkAfter));

st.stop();

