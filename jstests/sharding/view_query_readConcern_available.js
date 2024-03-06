/**
 * Tests queries on views on sharded collections work correctly with readConcern 'available'.
 */

const st = new ShardingTest({
    shards: 2,
});
const dbName = jsTestName();
const testDB = st.s.getDB(dbName);
const collName = "test";
const fullCollNs = `${dbName}.${collName}`;
const testColl = testDB.getCollection(collName);
const viewName = "myView";

// Shards a collection across the two shards and build a view on top of it.
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(testDB.adminCommand({shardCollection: fullCollNs, key: {x: 1}}));
assert.commandWorked(st.splitAt(fullCollNs, {x: 10}));
assert.commandWorked(st.moveChunk(fullCollNs, {x: 10}, st.shard1.shardName));
assert.commandWorked(testColl.insert({x: 100}));
assert.commandWorked(testColl.insert({x: -100}));
assert.commandWorked(testDB.createView(viewName, collName, []));

// The command from the router with 'available' readConcern should still retrieve both documents
// across the two shards.
assert.eq(testDB.getCollection(viewName).find().readConcern('available').itcount(), 2);

// Connecting to the primary shard will return the one document there.
assert.eq(st.shard0.getDB(dbName).getCollection(viewName).find().readConcern('available').itcount(),
          1);

// Connecting to the non-primary shard will not find the view and return nothing.
assert.eq(st.shard1.getDB(dbName).getCollection(viewName).find().readConcern('available').itcount(),
          0);

st.stop();