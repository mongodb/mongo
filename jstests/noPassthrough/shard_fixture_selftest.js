// A test to ensure that shard_fixture.js is consistent with shardingtest.js
// @tags: [
//   requires_sharding,
// ]

(function() {
'use strict';

load('jstests/concurrency/fsm_libs/shard_fixture.js');

const rsTestOriginal = new ShardingTest({shards: 2, mongos: 2, config: 2});

const rsTestWrapper =
    new FSMShardingTest(`mongodb://${rsTestOriginal.s0.host},${rsTestOriginal.s1.host}`);

assert.eq(rsTestWrapper.s(0).host, rsTestOriginal.s0.host);
assert.eq(rsTestWrapper.s(1).host, rsTestOriginal.s1.host);
assert.eq(rsTestWrapper.s(2), rsTestOriginal.s2);  // Both should be undefined.

assert.eq(rsTestWrapper.shard(0).host, rsTestOriginal.shard0.host);
assert.eq(rsTestWrapper.shard(1).host, rsTestOriginal.shard1.host);
assert.eq(rsTestWrapper.shard(2), rsTestOriginal.shard2);  // Both should be undefined.

assert.eq(rsTestWrapper.rs(0).getURL(), rsTestOriginal.rs0.getURL());
assert.eq(rsTestWrapper.rs(1).getURL(), rsTestOriginal.rs1.getURL());
assert.eq(rsTestWrapper.rs(2), rsTestOriginal.rs2);  // Both should be undefined.

assert.eq(rsTestWrapper.d(0), rsTestOriginal.d0);  // Both should be undefined.

assert.eq(rsTestWrapper.c(0).host, rsTestOriginal.c0.host);
assert.eq(rsTestWrapper.c(1).host, rsTestOriginal.c1.host);
assert.eq(rsTestWrapper.c(2), rsTestOriginal.c2);  // Both should be undefined.

rsTestOriginal.stop();
})();
