// Tests whether profiling can trigger stale config errors and interfere with write batches
// SERVER-13413
// @tags: [
//   # Test doesn't start enough mongods to have num_mongos routers
//   temp_disabled_embedded_router,
// ]

var st = new ShardingTest({shards: 1, mongos: 2});
st.stopBalancer();

var admin = st.s0.getDB('admin');
var coll = st.s0.getCollection('foo.bar');

assert(admin.runCommand({enableSharding: coll.getDB() + ''}).ok);
assert(admin.runCommand({shardCollection: coll + '', key: {_id: 1}}).ok);

st.printShardingStatus();

jsTest.log('Turning on profiling on ' + st.shard0);

st.shard0.getDB(coll.getDB().toString()).setProfilingLevel(2);

var profileColl = st.shard0.getDB(coll.getDB().toString()).system.profile;

var inserts = [{_id: 0}, {_id: 1}, {_id: 2}];

assert.commandWorked(st.s1.getCollection(coll.toString()).insert(inserts));

let profileEntry = profileColl.findOne({"op": "insert", "ns": coll.getFullName()});
assert.neq(null, profileEntry);
printjson(profileEntry);
assert.eq(profileEntry.command.documents, inserts);

st.stop();
