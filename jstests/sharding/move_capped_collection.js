/**
 * Tests for basic functionality of the move collection feature.
 *
 * @tags: [
 *  featureFlagMoveCollection,
 *  assumes_balancer_off,
 *  requires_capped
 * ]
 */

const st = new ShardingTest({shards: 2});

const dbName = 'db';
const collName = 'foo';
const ns = dbName + '.' + collName;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

assert.commandWorked(st.s.getDB(dbName).createCollection(collName, {capped: true, size: 4096}));

assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({data: 1}));

assert.commandWorked(st.s.adminCommand({moveCollection: ns, toShard: st.shard1.shardName}));

let collEntry = st.s.getDB('config').getCollection('collections').findOne({_id: ns});
assert.eq(collEntry._id, ns);
assert.eq(collEntry.unsplittable, true);
assert.eq(collEntry.key, {_id: 1});
assert.eq(st.s.getDB(dbName).getCollection(collName).isCapped(), true);
assert.eq(st.s.getDB(dbName).getCollection(collName).countDocuments({}), 1);
assert.eq(st.rs0.getPrimary().getDB(dbName).getCollection(collName).countDocuments({}), 0);
assert.eq(st.rs1.getPrimary().getDB(dbName).getCollection(collName).countDocuments({}), 1);

st.stop();
