var st = new ShardingTest({ shards: 3, chunksize: 1 });

var testDB = st.s.getDB('test');

// Disable the balancer to not interfere with the test, but keep the balancer settings on
// so the auto split logic will be able to move chunks around.
st.startBalancer();
var res = testDB.adminCommand({ configureFailPoint: 'skipBalanceRound', mode: 'alwaysOn' });

testDB.adminCommand({ enableSharding: 'test' });
testDB.adminCommand({ movePrimary: 'test', to: 'shard0001' });

// Basic test with no tag, top chunk should move to shard with least chunks.
//
// Setup:
// s0: [0, inf) -> 10 chunks
// s1: [-100, 0) -> 10 chunks
// s2: [-inf, -100) -> 1 chunk

testDB.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});

testDB.adminCommand({ split: 'test.user', middle: { x: 0 }});
assert.commandWorked(
    testDB.adminCommand({ moveChunk: 'test.user', find: { x: 0 }, to: 'shard0000' }));

for (var x = -100; x < 100; x+= 10) {
    testDB.adminCommand({ split: 'test.user', middle: { x: x }});
}

assert.commandWorked(
    testDB.adminCommand({ moveChunk: 'test.user', find: { x: -1000 }, to: 'shard0002' }));

var configDB = st.s.getDB('config');
var largeStr = new Array(1024).join('x');

// The inserts should be bulked as one so the auto-split will only be triggered once.
var bulk = testDB.user.initializeUnorderedBulkOp();
for (var x = 100; x < 2000; x++) {
    bulk.insert({ x: x, val: largeStr });
}
bulk.execute();

var topChunkAfter = configDB.chunks.find({}).sort({ min: -1 }).next();
assert.eq('shard0002', topChunkAfter.shard, 'chunk in the wrong shard: ' + tojson(topChunkAfter));

testDB.user.drop();

// Basic test with tag, top chunk should move to the other shard with the right tag.
//
// Setup:
// s0: [0, inf) -> 10 chunks, tag: A
// s1: [-100, 0) -> 10 chunks, tag: A
// s2: [-inf, -100) -> 1 chunk, tag: B

testDB.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});

testDB.adminCommand({ split: 'test.user', middle: { x: 0 }});
assert.commandWorked(
    testDB.adminCommand({ moveChunk: 'test.user', find: { x: 0 }, to: 'shard0000' }));

for (var x = -20; x < 100; x+= 10) {
    testDB.adminCommand({ split: 'test.user', middle: { x: x }});
}

assert.commandWorked(
    testDB.adminCommand({ moveChunk: 'test.user', find: { x: -1000 }, to: 'shard0002' }));

// assign global db variable to make sh.addShardTag work correctly.
db = testDB;

sh.addShardTag('shard0000', 'A');
sh.addShardTag('shard0001', 'A');
sh.addShardTag('shard0002', 'B');

sh.addTagRange('test.user', { x: MinKey }, { x: -100 }, 'B');
sh.addTagRange('test.user', { x: -100 }, { x: MaxKey }, 'A');

// The inserts should be bulked as one so the auto-split will only be triggered once.
bulk = testDB.user.initializeUnorderedBulkOp();
for (var x = 100; x < 2000; x++) {
    bulk.insert({ x: x, val: largeStr });
}
bulk.execute();

topChunkAfter = configDB.chunks.find({}).sort({ min: -1 }).next();
assert.eq('shard0001', topChunkAfter.shard, 'chunk in the wrong shard: ' + tojson(topChunkAfter));

st.stop();

