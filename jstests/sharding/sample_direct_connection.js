// This test ensures that an aggregation pipeline with a $sample stage can be issued through a
// direct connection to a shard without failing, and $sample behaves as if we sampled an unsharded
// collection.
// @tags: [requires_fcv_51]
(function() {
let st = new ShardingTest({shards: 1});

assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
assert.commandWorked(st.s.adminCommand({shardCollection: 'test.sharded', key: {x: 1}}));
const testDB = st.s.getDB('test');

// We must have >100 samples to attempt the storage optimized sample path.
for (let x = 0; x < 101; x++) {
    assert.commandWorked(testDB.foo.insert({x: x}));
}

const shardDB = st.rs0.getPrimary().getDB('test');

const res = assert.commandWorked(
    shardDB.runCommand({aggregate: 'foo', pipeline: [{$sample: {size: 3}}], cursor: {}}));
assert.eq(res.cursor.firstBatch.length, 3);

st.stop();
})();
