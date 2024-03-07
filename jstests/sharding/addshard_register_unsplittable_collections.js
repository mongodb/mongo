/**
 * Checks that addShard succesfully register any unsharded collection as an unsplitted collection.
 *
 * @tags: [
 *   featureFlagTrackUnshardedCollectionsUponCreation,
 *   multiversion_incompatible,
 *   assumes_balancer_off,
 * ]
 */

const st = new ShardingTest({shards: 2});

const kDb1 = 'AddShardDB1';
const kDb2 = 'AddShardDB2';
const kColl1 = 'coll1';
const kColl2 = 'coll2';
const kColl3 = 'coll3';
const kNss1 = kDb1 + '.' + kColl1;
const kNssDB2Coll1 = kDb2 + '.' + kColl1;
const kNss2 = kDb2 + '.' + kColl2;
const kNss3 = kDb2 + '.system.buckets.' + kColl3;

const kShard3Name = st.shard0.shardName + '-rs3';
const shard3 = new ReplSetTest({name: kShard3Name, nodes: 3});
shard3.startSet({shardsvr: ""});
shard3.initiate();

let newShardPrimary = shard3.getPrimary();  // Wait for there to be a primary

newShardPrimary.getDB(kDb1).runCommand({create: kColl1});
newShardPrimary.getDB(kDb2).runCommand({create: kColl1});
newShardPrimary.getDB(kDb2).runCommand({create: kColl2});
newShardPrimary.getDB(kDb2).runCommand({create: kColl3, timeseries: {timeField: 'time'}});

const shard3SeedList = shard3.name + "/" + shard3.nodes[0].host;

st.s.getDB(kDb2).getCollection(kColl1).insert({x: 1});

// Existing DB and collection found.
assert.commandFailedWithCode(st.admin.runCommand({addshard: shard3SeedList, name: kShard3Name}),
                             ErrorCodes.OperationFailed);
assert.commandWorked(st.s.getDB(kDb2).dropDatabase());

assert.commandWorked(st.admin.runCommand({addshard: shard3SeedList, name: kShard3Name}));

assert.eq(1, st.s.getCollection('config.collections').countDocuments({_id: kNss1}));
assert.eq(st.s.getCollection('config.collections').countDocuments({_id: kNssDB2Coll1}), 1);
assert.eq(st.s.getCollection('config.collections').countDocuments({_id: kNss2}), 1);
assert.eq(st.s.getCollection('config.collections').countDocuments({_id: kNss3}), 1);
assert.commandWorked(st.s.getDB(kDb1).dropDatabase());
assert.commandWorked(st.s.getDB(kDb2).dropDatabase());

shard3.stopSet();
st.stop();
