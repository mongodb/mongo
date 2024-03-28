/**
 * Tests the new _shardsvrUntrackCollection command
 * @tags: [
 *   requires_fcv_80
 * ]
 */

const kDbName = 'db';
const kCollName = 'coll';
const kNss = kDbName + '.' + kCollName;

const st = new ShardingTest({mongos: 1, shards: 2, config: 1});
const shard0Name = st.shard0.shardName;
const shard1Name = st.shard1.shardName;
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName, primaryShard: shard0Name}));
const db = st.s.getCollection('config.databases').findOne({_id: kDbName});

assert.commandWorked(st.s.getCollection(kNss).insert({x: 1}));

// Trying to untrack a non tracked collection is a noop.
assert.commandWorked(st.rs0.getPrimary().adminCommand({
    _shardsvrUntrackUnsplittableCollection: kNss,
    writeConcern: {w: 'majority'},
    databaseVersion: db.version
}));

// Track and move the collection to shard 1.
assert.commandWorked(st.s.adminCommand({moveCollection: kNss, toShard: shard1Name}));

// Save uuid for future checks.
const collUUID = st.s.getCollection('config.collections').findOne({_id: kNss}).uuid;

// Can't untrack collection if it's not in the primary shard.
assert.commandFailedWithCode(st.rs0.getPrimary().adminCommand({
    _shardsvrUntrackUnsplittableCollection: kNss,
    writeConcern: {w: 'majority'},
    databaseVersion: db.version
}),
                             ErrorCodes.OperationFailed);
assert.commandWorked(st.s.adminCommand({moveCollection: kNss, toShard: shard0Name}));
assert.commandWorked(st.rs0.getPrimary().adminCommand({
    _shardsvrUntrackUnsplittableCollection: kNss,
    writeConcern: {w: 'majority'},
    databaseVersion: db.version
}));

// Make sure there is no entry on config.collections.
assert.eq(0, st.s.getCollection('config.collections').countDocuments({_id: kNss}));
// Make sure there is no entry on config.chunks.
assert.eq(0, st.s.getCollection('config.chunks').countDocuments({uuid: collUUID}));

st.stop();
