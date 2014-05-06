//
// Tests that multi-writes (update/delete) target *all* shards and not just shards in the collection
//

var options = { separateConfig : true };

var st = new ShardingTest({ shards : 3, mongos : 2, other : options });
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB( "admin" );
var shards = mongos.getCollection( "config.shards" ).find().toArray();
var coll = mongos.getCollection( "foo.bar" );

assert( admin.runCommand({ enableSharding : coll.getDB() + "" }).ok );
printjson( admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }) );
assert( admin.runCommand({ shardCollection : coll + "", key : { skey : 1 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { skey : 0 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { skey : 100 } }).ok );
assert( admin.runCommand({ moveChunk : coll + "",
                           find : { skey : 0 },
                           to : shards[1]._id }).ok );
assert( admin.runCommand({ moveChunk : coll + "",
                           find : { skey : 100 },
                           to : shards[2]._id }).ok );

st.printShardingStatus();

jsTest.log("Testing multi-update...");

// Put data on all shards
assert.writeOK(st.s0.getCollection(coll.toString()).insert({ _id : 0, skey : -1, x : 1 }));
assert.writeOK(st.s0.getCollection(coll.toString()).insert({ _id : 1, skey : 1, x : 1 }));
assert.writeOK(st.s0.getCollection(coll.toString()).insert({ _id: 0, skey: 100, x: 1 }));

// Non-multi-update doesn't work without shard key
assert.writeError(coll.update({ x : 1 }, { $set : { updated : true } }, { multi : false }));

assert.writeOK(coll.update({ x : 1 }, { $set : { updated : true } }, { multi : true }));

// Ensure update goes to *all* shards
assert.neq(null, st.shard0.getCollection(coll.toString()).findOne({ updated : true }));
assert.neq(null, st.shard1.getCollection(coll.toString()).findOne({ updated : true }));
assert.neq(null, st.shard2.getCollection(coll.toString()).findOne({ updated : true }));

// _id update works, and goes to all shards even on the stale mongos
var staleColl = st.s1.getCollection('foo.bar');
assert.writeOK(staleColl.update({ _id : 0 }, { $set : { updatedById : true } }, { multi : false }));

// Ensure _id update goes to *all* shards
assert.neq(null, st.shard0.getCollection(coll.toString()).findOne({ updatedById : true }));
assert.neq(null, st.shard2.getCollection(coll.toString()).findOne({ updatedById : true }));

jsTest.log("Testing multi-delete...");

// non-multi-delete doesn't work without shard key
assert.writeError(coll.remove({ x : 1 }, { justOne : true }));

assert.writeOK(coll.remove({ x : 1 }, { justOne : false }));

// Ensure delete goes to *all* shards
assert.eq(null, st.shard0.getCollection(coll.toString()).findOne({ x : 1 }));
assert.eq(null, st.shard1.getCollection(coll.toString()).findOne({ x : 1 }));
assert.eq(null, st.shard2.getCollection(coll.toString()).findOne({ x : 1 }));

// Put more on all shards
assert.writeOK(st.shard0.getCollection(coll.toString()).insert({ _id : 0, skey : -1, x : 1 }));
assert.writeOK(st.shard1.getCollection(coll.toString()).insert({ _id : 1, skey : 1, x : 1 }));
// Data not in chunks
assert.writeOK(st.shard2.getCollection(coll.toString()).insert({ _id : 0, x : 1 }));

assert.writeOK(coll.remove({ _id : 0 }, { justOne : true }));

// Ensure _id delete goes to *all* shards
assert.eq(null, st.shard0.getCollection(coll.toString()).findOne({ x : 1 }));
assert.eq(null, st.shard2.getCollection(coll.toString()).findOne({ x : 1 }));

jsTest.log( "DONE!" );

st.stop();
