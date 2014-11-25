/**
 * Tests setShardVersion, particularly on the case where mongos sends it to a
 * shard that does not have any chunks.
 */

var st = new ShardingTest({ shards: 2, mongos: 2 });
st.stopBalancer();

var configDB = st.s.getDB('config');
configDB.adminCommand({ enableSharding: 'test' });
configDB.adminCommand({ movePrimary: 'test', to: 'shard0000' });
configDB.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});

var testDB = st.s.getDB('test');

testDB.user.insert({ x: 1 });
testDB.runCommand({ getLastError: 1 });

var doc = testDB.user.findOne();

var testDB2 = st.s1.getDB('test');

configDB.adminCommand({ moveChunk: 'test.user', find: { x: 0 }, to: 'shard0001' });

assert.eq(1, testDB.user.find().itcount());
assert.eq(1, testDB2.user.find().itcount());

assert.eq(1, testDB.user.find({ x: 1 }).itcount());
assert.eq(1, testDB2.user.find({ x: 1 }).itcount());

var configDB2 = st.s1.getDB('config');
configDB2.adminCommand({ moveChunk: 'test.user', find: { x: 0 }, to: 'shard0000' });

assert.eq(1, testDB.user.find().itcount());
assert.eq(1, testDB2.user.find().itcount());

assert.eq(1, testDB.user.find({ x: 1 }).itcount());
assert.eq(1, testDB2.user.find({ x: 1 }).itcount());

st.stop();

