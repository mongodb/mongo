/**
 * Test that unsplittable collections are removed from the sharding catalog upon downgrade.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */
const st = new ShardingTest({shards: 2});
const mongos = st.s;

const primaryShardName = st.shard0.shardName;
const nonPrimaryShardName = st.shard1.shardName;

const dbName = "test";
const dbNameRegExp = RegExp("^" + dbName);

mongos.adminCommand({enableSharding: dbName, primaryShard: primaryShardName});
const db = mongos.getDB(dbName);

const collNeverTracked = db.getCollection('neverTracked');
const collMoved = db.getCollection("moved");
const collToMove = db.getCollection("toMove");
const collSharded = db.getCollection("sharded");
const testCollections = [collNeverTracked, collMoved, collToMove, collSharded];

// Collection not tracked
assert.commandWorked(db.runCommand({create: collNeverTracked.getName()}));
// Collection tracked on the primary shard
assert.commandWorked(db.runCommand({create: collMoved.getName()}));
assert.commandWorked(
    mongos.adminCommand({moveCollection: collMoved.getFullName(), toShard: nonPrimaryShardName}));
assert.commandWorked(
    mongos.adminCommand({moveCollection: collMoved.getFullName(), toShard: primaryShardName}));
// Collection tracked outside the primary shard
assert.commandWorked(db.runCommand({create: collToMove.getName()}));
assert.commandWorked(
    mongos.adminCommand({moveCollection: collToMove.getFullName(), toShard: nonPrimaryShardName}));
// Sharded collection (must not be untracked)
assert.commandWorked(
    mongos.adminCommand({shardCollection: collSharded.getFullName(), key: {_id: 'hashed'}}));

// The sharded collection and the unsplittable collections must be tracked
const configCollections = mongos.getCollection('config.collections');
assert.eq(3, configCollections.countDocuments({_id: dbNameRegExp}));
assert.eq(2, configCollections.countDocuments({_id: dbNameRegExp, unsplittable: true}));

// Insert a document in all collections
const doc = {
    x: "foo"
};
testCollections.forEach(coll => {
    coll.insert(doc);
});

jsTest.log(
    'Test that downgrade fails if there is an unsplittable collection outside the primary shard');

assert.commandFailedWithCode(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    ErrorCodes.CannotDowngrade);

// Set FCV back to `latest` to be able to move back all collections to the primary shard.
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
assert.commandWorked(
    st.s.adminCommand({moveCollection: collToMove.getFullName(), toShard: primaryShardName}));

jsTest.log('Test that downgrade succeeds after untracking unsplittable collections');
assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

// Only the sharded collection must be tracked
assert.eq(1, configCollections.countDocuments({_id: dbNameRegExp}));
assert.eq(0, configCollections.countDocuments({_id: dbNameRegExp, unsplittable: true}));

// Make sure no data were dropped as result of untracking collections
testCollections.forEach(coll => {
    assert.eq(1, coll.countDocuments(doc));
});

st.stop();
