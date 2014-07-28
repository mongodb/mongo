// Test for splitting a chunk with a very large shard key value should not be allowed
// and does not corrupt the config.chunks metadata.

var st = new ShardingTest({ shards: 1 });
var configDB = st.s.getDB('config');

configDB.adminCommand({ enableSharding: 'test' });
configDB.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});

var str1k = new Array(512).join('a');
var res = configDB.adminCommand({ split: 'test.user', middle: { x: str1k }});
assert(!res.ok);
assert(res.errmsg != null);

// Verify chunk document
assert.eq(1, configDB.chunks.find().count());
var chunkDoc = configDB.chunks.findOne();
assert.eq(0, bsonWoCompare(chunkDoc.min, { x: MinKey }));
assert.eq(0, bsonWoCompare(chunkDoc.max, { x: MaxKey }));

configDB.adminCommand({ shardCollection: 'test.user2', key: { x: 1, y: 1 }});
var strHalfk = new Array(256).join('a');
res = configDB.adminCommand({ split: 'test.user2', middle: { x: strHalfk, y: strHalfk }});
assert(!res.ok);
assert(res.errmsg != null);

// Verify chunk document
assert.eq(1, configDB.chunks.find({ ns: 'test.user2' }).count());
chunkDoc = configDB.chunks.findOne({ ns: 'test.user2' });
assert.eq(0, bsonWoCompare(chunkDoc.min, { x: MinKey, y: MinKey }));
assert.eq(0, bsonWoCompare(chunkDoc.max, { x: MaxKey, y: MaxKey }));

st.stop();

