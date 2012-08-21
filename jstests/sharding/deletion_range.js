//
// Tests deletion ranges for a sharded system when using prefix shard key
//

var st = new ShardingTest({ shards : 2, mongos : 2 });

st.stopBalancer();

var mongos = st.s0;
var config = mongos.getDB( "config" );
var admin = mongos.getDB( "admin" );
var shards = config.shards.find().toArray();
var shard0 = new Mongo( shards[0].host );
var shard1 = new Mongo( shards[1].host );

var coll = mongos.getCollection( "foo.bar" );

printjson( admin.runCommand({ enableSharding : coll.getDB() + "" }) );
printjson( admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }) );
printjson( coll.ensureIndex({ skey : 1, extra : 1 }) );
printjson( admin.runCommand({ shardCollection : coll + "", key : { skey : 1 } }) );

for( var i = 0; i < 5; i++ ){
    coll.insert({ skey : 0, extra : i });
}
assert.eq( null, coll.getDB().getLastError() );

printjson( admin.runCommand({ split : coll + "", middle : { skey : 0 } }) );
printjson( admin.runCommand({ moveChunk : coll + "", find : { skey : 0 }, to : shards[1]._id }) );

printjson( shard0.getCollection( coll + "" ).find().toArray() );
printjson( shard1.getCollection( coll + "" ).find().toArray() );

assert( coll.find().itcount() == 5 );

printjson( admin.runCommand({ moveChunk : coll + "", find : { skey : -1 }, to : shards[1]._id }) );

assert.eq( 0 , shard0.getCollection( coll + "" ).find().itcount() );
assert.eq( 5 , shard1.getCollection( coll + "" ).find().itcount() );

assert( coll.find().itcount() == 5 );

st.stop()