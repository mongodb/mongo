//
// Tests that dropping and re-adding a shard with the same name to a cluster doesn't mess up
// migrations
//

var st = new ShardingTest({ shards : 3, mongos : 1, other : { separateConfig : true } })
st.stopBalancer()

var mongos = st.s
var admin = mongos.getDB( "admin" )
var config = mongos.getDB( "config" )
var coll = mongos.getCollection( "foo.bar" )

// Get all the shard info and connections
var shards = []
config.shards.find().sort({ _id : 1 }).forEach( function( doc ){ 
    shards.push( Object.merge( doc, { conn : new Mongo( doc.host ) } ) )
})

//
// Remove the last shard so we can use it later
//

// Drain & remove
printjson( admin.runCommand({ removeShard : shards[2]._id }) )
printjson( admin.runCommand({ removeShard : shards[2]._id }) )

// Shard collection
printjson( admin.runCommand({ enableSharding : coll.getDB() + "" }) )
// Just to be sure what primary we start from
printjson( admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }) )
printjson( admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }) )

// Insert one document
coll.insert({ hello : "world" })
assert.eq( null, coll.getDB().getLastError() )

// Migrate the collection to and from shard2 so shard1 loads the shard2 host
printjson( admin.runCommand({ moveChunk : coll + "", find : { _id : 0 }, to : shards[1]._id, _waitForDelete : true }) )
printjson( admin.runCommand({ moveChunk : coll + "", find : { _id : 0 }, to : shards[0]._id, _waitForDelete : true }) )

//
// Drop and re-add shard with last shard's host
//

printjson( admin.runCommand({ removeShard : shards[1]._id }) )
printjson( admin.runCommand({ removeShard : shards[1]._id }) )
printjson( admin.runCommand({ addShard : shards[2].host, name : shards[1]._id }) )

jsTest.log( "Shard was dropped and re-added with same name..." )
st.printShardingStatus()

shards[0].conn.getDB( "admin" ).runCommand({ setParameter : 1, traceExceptions : true })
shards[2].conn.getDB( "admin" ).runCommand({ setParameter : 1, traceExceptions : true })

// Try a migration
printjson( admin.runCommand({ moveChunk : coll + "", find : { _id : 0 }, to : shards[1]._id }) )

assert.neq( null, shards[2].conn.getCollection( coll + "" ).findOne() )

st.stop()


