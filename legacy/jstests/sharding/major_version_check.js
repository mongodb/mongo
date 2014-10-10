//
// Tests that only a correct major-version is needed to connect to a shard via mongos
//

var st = new ShardingTest({ shards : 1, mongos : 2, other : { separateConfig : true } })
st.stopBalancer()

var mongos = st.s0
var staleMongos = st.s1
var admin = mongos.getDB( "admin" )
var config = mongos.getDB( "config" )
var coll = mongos.getCollection( "foo.bar" )

// This converter is needed as spidermonkey and v8 treat Timestamps slightly differently
// TODO: Is this a problem? SERVER-6079
var tsToObj = function( obj ){
    return { t : obj.t, i : obj.i }
}

// Shard collection
printjson( admin.runCommand({ enableSharding : coll.getDB() + "" }) )
printjson( admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }) )

// Make sure our stale mongos is up-to-date with no splits
staleMongos.getCollection( coll + "" ).findOne()

// Run one split
printjson( admin.runCommand({ split : coll + "", middle : { _id : 0 } }) )

// Make sure our stale mongos is not up-to-date with the split
printjson( admin.runCommand({ getShardVersion : coll + "" }) )
printjson( staleMongos.getDB( "admin" ).runCommand({ getShardVersion : coll + "" }) )

// Compare strings b/c timestamp comparison is a bit weird
assert.eq( tsToObj( Timestamp( 1, 2 ) ), 
           tsToObj( admin.runCommand({ getShardVersion : coll + "" }).version ) )
assert.eq( tsToObj( Timestamp( 1, 0 ) ), 
           tsToObj( staleMongos.getDB( "admin" ).runCommand({ getShardVersion : coll + "" }).version ) )

// See if our stale mongos is required to catch up to run a findOne on an existing connection
staleMongos.getCollection( coll + "" ).findOne()

printjson( staleMongos.getDB( "admin" ).runCommand({ getShardVersion : coll + "" }) )

assert.eq( tsToObj( Timestamp( 1, 0 ) ), 
           tsToObj( staleMongos.getDB( "admin" ).runCommand({ getShardVersion : coll + "" }).version ) )
           
// See if our stale mongos is required to catch up to run a findOne on a new connection
staleMongos = new Mongo( staleMongos.host )
staleMongos.getCollection( coll + "" ).findOne()

printjson( staleMongos.getDB( "admin" ).runCommand({ getShardVersion : coll + "" }) )

assert.eq( tsToObj( Timestamp( 1, 0 ) ), 
           tsToObj( staleMongos.getDB( "admin" ).runCommand({ getShardVersion : coll + "" }).version ) )

st.stop()