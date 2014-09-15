// Test hashed presplit with 1 shard.

var st = new ShardingTest({ shards: 1 });
var testDB = st.getDB('test');

//create hashed shard key and enable sharding
testDB.adminCommand({ enablesharding: "test" });
testDB.adminCommand({ shardCollection: "test.collection", key: { a: "hashed" }});

//check the number of initial chunks.
assert.eq(2, st.getDB('config').chunks.count(),
    'Using hashed shard key but failing to do correct presplitting');
st.stop();

