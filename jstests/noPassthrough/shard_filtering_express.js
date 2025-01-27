/**
 * Verify that simple equality predicates against compound unique
 * indexes filter orphans correctly.
 *
 * Previously these queries were Express eligible and were known to
 * produce incorrect responses (see SERVER-97860).
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */

// Deliberately inserts orphans outside of migration.
TestData.skipCheckOrphans = true;
const st = new ShardingTest({shards: 2});
const collName = "test.shardfilter";
const mongosDb = st.s.getDB("test");
const mongosColl = st.s.getCollection(collName);

assert.commandWorked(st.s.adminCommand({enableSharding: "test", primaryShard: st.shard1.name}));
assert.commandWorked(
    st.s.adminCommand({shardCollection: collName, key: {a: 1, b: 1}, unique: true}));

// shard0 gets small chunk so that we can create orphans on shard1
assert.commandWorked(st.s.adminCommand({split: collName, middle: {a: 1, b: 10}}));
assert.commandWorked(st.s.adminCommand({split: collName, middle: {a: 1, b: 20}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: collName, find: {a: 1, b: 15}, to: st.shard0.shardName}));

const orphanDocs = [
    {_id: 9, a: 1, b: 10},
];
assert.commandWorked(st.shard1.getCollection(collName).insert(orphanDocs));

// Insert legit doc.
assert.commandWorked(mongosColl.insert([
    {_id: 2, a: 1, b: 22},
]));

assert.eq(mongosColl.find().itcount(), 1);
assert.eq(mongosColl.find({a: 1}).limit(1).toArray(), [{_id: 2, a: 1, b: 22}]);

st.stop();
