//
// Tests that stale mongoses can properly move chunks.
//

var st = new ShardingTest({shards: 2, mongos: 2});
var admin = st.s0.getDB('admin');
var testDb = 'test';
var testNs = 'test.foo';
var shards = st.getShardNames();

assert.commandWorked(admin.runCommand({enableSharding: testDb}));
st.ensurePrimaryShard(testDb, shards[0]);
assert.commandWorked(admin.runCommand({shardCollection: testNs, key: {_id: 1}}));
var curShardIndex = 0;

for (var i = 0; i < 100; i += 10) {
    assert.commandWorked(st.s0.getDB('admin').runCommand({split: testNs, middle: {_id: i}}));
    var nextShardIndex = (curShardIndex + 1) % shards.length;
    assert.commandWorked(st.s1.getDB('admin').runCommand(
        {moveChunk: testNs, find: {_id: i + 5}, to: shards[nextShardIndex]}));
    curShardIndex = nextShardIndex;
}

st.stop();