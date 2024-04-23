const dbName = 'test';
const collName = 'foo';

const st = new ShardingTest({mongos: 1, shards: 2});
const db = st.s.getDB(dbName);
const coll = db[collName]

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Create a capped collection.
assert.commandWorked(db.createCollection(collName, {capped: true, size: 1000}));

// Insert more than one document to it.
assert.commandWorked(coll.insertMany([{x: 0}, {x: 1}]));

// Move the collection.
assert.commandWorked(
    st.s.adminCommand({moveCollection: coll.getFullName(), toShard: st.shard1.shardName}));

st.stop();
