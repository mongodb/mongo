//
// Tests that stale mongoses can properly move chunks.
//

var st = new ShardingTest({shards: 2, mongos: 2});
var admin = st.s0.getDB('admin');
var testDb = 'test';
var testNs = 'test.foo';

assert.commandWorked(admin.runCommand({enableSharding: testDb}));
st.ensurePrimaryShard(testDb, st.shard0.shardName);
assert.commandWorked(admin.runCommand({shardCollection: testNs, key: {_id: 1}}));
var curShardIndex = 0;

for (var i = 0; i < 100; i += 10) {
    assert.commandWorked(st.s0.getDB('admin').runCommand({split: testNs, middle: {_id: i}}));
    st.configRS.awaitLastOpCommitted();  // Ensure that other mongos sees the split
    var nextShardIndex = (curShardIndex + 1) % 2;
    var toShard = (nextShardIndex == 0) ? st.shard0.shardName : st.shard1.shardName;
    assert.commandWorked(st.s1.getDB('admin').runCommand(
        {moveChunk: testNs, find: {_id: i + 5}, to: toShard, _waitForDelete: true}));
    curShardIndex = nextShardIndex;
    st.configRS.awaitLastOpCommitted();  // Ensure that other mongos sees the move
}

st.stop();
