//
// Tests that stale bongoses can properly split chunks.
//

var st = new ShardingTest({shards: 2, bongos: 2});
var admin = st.s0.getDB('admin');
var testDb = 'test';
var testNs = 'test.foo';

assert.commandWorked(admin.runCommand({enableSharding: testDb}));
assert.commandWorked(admin.runCommand({shardCollection: testNs, key: {_id: 1}}));

for (var i = 0; i < 100; i += 10) {
    assert.commandWorked(st.s0.getDB('admin').runCommand({split: testNs, middle: {_id: i}}));
    st.configRS.awaitLastOpCommitted();  // Ensure that other bongos sees the previous split
    assert.commandWorked(st.s1.getDB('admin').runCommand({split: testNs, middle: {_id: i + 5}}));
    st.configRS.awaitLastOpCommitted();  // Ensure that other bongos sees the previous split
}

st.stop();