/**
 * Test that migration should fail if the destination shard does not
 * have the index and is not empty.
 */

var st = new ShardingTest({ shards: 2 });

var testDB = st.s.getDB('test');

testDB.adminCommand({ enableSharding: 'test' });
testDB.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});

testDB.user.ensureIndex({ a: 1, b: 1 });
testDB.user.ensureIndex({ z: 'hashed' });

testDB.adminCommand({ split: 'test.user', middle: { x: 0 }});
testDB.adminCommand({ split: 'test.user', middle: { x: 10 }});

assert.commandWorked(testDB.adminCommand({ moveChunk: 'test.user',
                                           find: { x: 0 },
                                           to: 'shard0000' }));

st.d0.getDB('test').user.dropIndex({ a: 1, b: 1 });

assert.commandWorked(testDB.adminCommand({ moveChunk: 'test.user',
                                           find: { x: 10 },
                                           to: 'shard0000' }));

st.d0.getDB('test').user.dropIndex({ a: 1, b: 1 });
testDB.user.insert({ x: 10 });

assert.commandFailed(testDB.adminCommand({ moveChunk: 'test.user',
                                           find: { x: -10 },
                                           to: 'shard0000' }));

st.stop();
