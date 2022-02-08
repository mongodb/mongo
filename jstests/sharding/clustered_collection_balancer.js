/**
 * Test inserts into a sharded clustered collection with the balancer on.
 *
 * @tags: [
 *   requires_fcv_53,
 *   # The inMemory variants may run out of memory while inserting large input objects.
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

Random.setRandomSeed();

const dbName = 'testDB';
const collName = 'testColl';

// Connections.
const st = new ShardingTest({shards: 2, rs: {nodes: 2}, other: {chunkSize: 1}});
const mongos = st.s0;

// Enable sharding on the database.
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
const mainDB = mongos.getDB(dbName);

const largeStr = "a".repeat(10000);
const clusteredCreateOpts = {
    clusteredIndex: {key: {_id: 1}, unique: true, name: "nameOnId"}
};

st.startBalancer();

function runTest(shardKey) {
    assert.commandWorked(mainDB.createCollection(collName, clusteredCreateOpts));
    let coll = mainDB.getCollection(collName);

    if (shardKey != "_id") {
        // Require creating an index when the shard key is not the cluster key.
        assert.commandWorked(coll.createIndex({[shardKey]: 1}));
    }

    // Insert a large dataset so that the balancer is guranteed to split the chunks.
    let bulk = coll.initializeUnorderedBulkOp();
    const numDocs = 1000;
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({[shardKey]: i * 100, largeField: largeStr});
    }
    assert.commandWorked(bulk.execute());

    assert.commandWorked(mongos.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: {[shardKey]: 1},
    }));
    st.awaitBalancerRound();

    // Ensure that each shard has at least one chunk after the split.
    const primaryShard = st.getPrimaryShard(dbName);
    const otherShard = st.getOther(primaryShard);
    assert.soon(
        () => {
            const counts = st.chunkCounts(collName, dbName);
            return counts[primaryShard.shardName] >= 1 && counts[otherShard.shardName] >= 1;
        },
        () => {
            return tojson(mongos.getDB("config").getCollection("chunks").find().toArray());
        });

    // Verify that all the documents still exist in the collection.
    assert.eq(coll.find().itcount(), numDocs);
    assert(coll.drop());
}

runTest("_id");
runTest("b");
st.stop();
})();
