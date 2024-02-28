/*
 * Tests that moveCollection works on unsplittable collections with unique indexes that are not
 * prefix of _id.
 * @tags: [
 *   featureFlagTrackUnshardedCollectionsOnShardingCatalog,
 *   featureFlagMoveCollection,
 *   featureFlagUnshardCollection,
 *   featureFlagReshardingImprovements,
 *   requires_fcv_72
 * ]
 */

(function() {
const st = new ShardingTest({shards: 2});

const kDbName = 'foo';
const kCollName = 'test';
const nss = kDbName + '.' + kCollName;
assert.commandWorked(
    st.s.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));

assert.commandWorked(st.s.getDB(kDbName).createCollection(kCollName));

assert.commandWorked(st.s.getCollection(nss).createIndex({oldKey: 1, a: 1, b: 1}, {unique: true}));

assert.commandWorked(st.s.adminCommand({moveCollection: nss, toShard: st.shard1.shardName}));

assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {oldKey: 1}}));
assert.commandFailedWithCode(st.s.adminCommand({reshardCollection: nss, key: {newKey: 1}}),
                             ErrorCodes.InvalidOptions);

assert.commandWorked(st.s.adminCommand({unshardCollection: nss, toShard: st.shard0.shardName}));

st.stop();
})();