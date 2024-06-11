/*
 * Test that when the 'uuid' field is missing from a config.cache.collections entry (from a
 * collection cached before v3.6), FCV upgrade to 5.0 succeeds and after it all persisted cache
 * entries have uuid.
 */
(function() {
'use strict';

const slowTestVariant = buildInfo().buildEnvironment.target_arch.includes('ppc');
const isStepdownSuite = typeof ContinuousStepdown != "undefined";

if (slowTestVariant && isStepdownSuite) {
    jsTestLog("Test times out in slow variants with continuous stepdown suites.");
    return;
}

const st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: '4.4'}));

assert.commandWorked(
    st.s.adminCommand({enableSharding: 'test', primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: 'test.foo', key: {x: 1}}));

// Leave shard0 without any chunk.
assert.commandWorked(
    st.s.adminCommand({moveChunk: 'test.foo', find: {x: 0}, to: st.shard1.shardName}));

// Simulate leftover config.cache.collections entries missing the 'uuid' field (as if they had been
// before on v3.6).
assert.commandWorked(
    st.rs0.getPrimary().getCollection('config.cache.collections').update({_id: 'test.foo'}, {
        $unset: {uuid: true}
    }));

assert.eq(1,
          st.rs0.getPrimary()
              .getCollection('config.cache.collections')
              .find({uuid: {$exists: false}})
              .itcount());

// FCV upgrade to v5.0 should work and after it finishes all config.cache.collection entries should
// have uuid.
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: '5.0'}));

// Assert that after fcv upgrade all collections have uuid.
assert.eq(0,
          st.rs0.getPrimary()
              .getCollection('config.cache.collections')
              .find({uuid: {$exists: false}})
              .itcount());

st.stop();
})();
