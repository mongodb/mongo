//
// Tests that we can only merge empty chunks
//

var options = { separateConfig : true, shardOptions : { verbose : 0 } };

var st = new ShardingTest({ shards : 2, mongos : 2, other : options });
st.stopBalancer();

var mongos = st.s0;
var staleMongos = st.s1;
var admin = mongos.getDB( "admin" );
var shards = mongos.getCollection( "config.shards" ).find().toArray();
var coll = mongos.getCollection( "foo.bar" );

assert( admin.runCommand({ enableSharding : coll.getDB() + "" }).ok );
printjson( admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }) );
assert( admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }).ok );

// Create ranges MIN->0, 0->10,10->20, 20->30,30->40, 40->50,50->60, 60->MAX
jsTest.log( "Creating ranges..." );

assert( admin.runCommand({ split : coll + "", middle : { _id : 0 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : 10 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : 20 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : 30 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : 40 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : 50 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : 60 } }).ok );

st.printShardingStatus();

// Insert data to allow 0->20 and 40->60 to be merged, but too much for 20->40
coll.insert({ _id : 0 });
coll.insert({ _id : 20 });
coll.insert({ _id : 30 });
coll.insert({ _id : 40 });
assert.eq( null, coll.getDB().getLastError() );

jsTest.log( "Merging chunks with another empty chunk..." );

assert( admin.runCommand({ mergeChunks : coll + "", 
                           bounds : [{ _id : 0 }, { _id : 20 }] }).ok );

assert( admin.runCommand({ mergeChunks : coll + "", 
                           bounds : [{ _id : 40 }, { _id : 60 }] }).ok );

jsTest.log( "Merging two full chunks should fail..." );

assert( !admin.runCommand({ mergeChunks : coll + "", 
                            bounds : [{ _id : 20 }, { _id : 40 }] }).ok );

st.printShardingStatus();

st.stop();

