//
// Tests whether profiling can trigger stale config errors and interfere with write batches
// SERVER-13413
//

var st = new ShardingTest({ shards : 1, mongos : 2 });
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB( "admin" );
var shards = mongos.getCollection( "config.shards" ).find().toArray();
var coll = mongos.getCollection( "foo.bar" );

assert( admin.runCommand({ enableSharding : coll.getDB() + "" }).ok );
assert( admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }).ok );

st.printShardingStatus();

jsTest.log( "Turning on profiling..." );

st.shard0.getDB(coll.getDB().toString()).setProfilingLevel(2);
var profileColl = st.shard0.getDB(coll.getDB().toString()).system.profile;

var inserts = [{ _id : 0 }, { _id : 1 }, { _id : 2 }];
var staleColl = st.s1.getCollection(coll.toString());

assert.writeOK(staleColl.insert(inserts));

printjson(profileColl.find().toArray());

for (var i = 0; i < inserts.length; i++) {
    assert.neq(null, profileColl.findOne({ 'query._id' : i }));
}

jsTest.log( "DONE!" );
st.stop();
