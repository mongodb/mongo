// Test that the shardCollection command fails when a preexisting document lacks a shard key field.
// SERVER-8772

var st = new ShardingTest({shards: 1});
st.stopBalancer();

var db = st.s.getDB('testDb');
var coll = db.testColl;

coll.insert({x: 1, z: 1});
coll.insert({y: 1, z: 1});
db.adminCommand({enableSharding: 'testDb'});

/**
 * Assert that the shardCollection command fails, with a preexisting index on the provided
 * 'shardKey'.
 */
function assertInvalidShardKey(shardKey) {
    // Manually create a shard key index.
    coll.dropIndexes();
    coll.ensureIndex(shardKey);

    // Ensure that the shard key index identifies 'x' as present in one document and absent in the
    // other.
    assert.eq(1, coll.find({x: 1}).hint(shardKey).itcount());
    assert.eq(1, coll.find({x: {$exists: false}}).hint(shardKey).itcount());

    // Assert that the shardCollection command fails with the provided 'shardKey'.
    assert.commandFailed(db.adminCommand({shardCollection: 'testDb.testColl', key: shardKey}),
                         'shardCollection should have failed on key ' + tojson(shardKey));
}

// Test single, compound, and hashed shard keys.
assertInvalidShardKey({x: 1});
assertInvalidShardKey({x: 1, y: 1});
assertInvalidShardKey({y: 1, x: 1});
assertInvalidShardKey({x: 'hashed'});

st.stop();
