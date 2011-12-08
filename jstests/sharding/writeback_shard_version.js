// Tests whether a newly sharded collection can be handled by the wbl

var jsTestLog = function( msg ){ print( "\n\n****\n" + msg + "\n****\n\n" ) }
var jsTestName = function(){ return "writeback_shard_version" }

jsTestLog( "Starting sharded cluster..." )

// Need to start as a replica set here, just because there's no other way to trigger separate configs,
// See SERVER-4222
var st = new ShardingTest( name = jsTestName(), shards = 1, verbose = 2, mongos = 2, other = { rs : true } )

var mongosA = st._mongos[0]
var mongosB = st._mongos[1]

var config = mongosA.getDB("config")
config.settings.update({ _id : "balancer" }, { $set : { stopped : true } }, true )

jsTestLog( "Adding new collections...")

var collA = mongosA.getCollection( jsTestName() + ".coll" )
collA.insert({ hello : "world" })
assert.eq( null, collA.getDB().getLastError() )

var collB = mongosB.getCollection( "" + collA )
collB.findOne()
collB.insert({ hello : "world" })
assert.eq( null, collB.getDB().getLastError() )

jsTestLog( "Enabling sharding..." )

printjson( mongosA.getDB( "admin" ).runCommand({ enableSharding : "" + collA.getDB() }) )

otherCollA = mongosA.getCollection( jsTestName() + ".otherColl" )
otherCollB = mongosB.getCollection( "" + otherCollA )
printjson( mongosA.getDB( "admin" ).runCommand({ shardCollection : "" + otherCollA, key : { _id : 1 } }) )

// Make sure mongosB knows about the collC sharding
mongosA.getDB("admin").runCommand({ flushRouterConfig : 1 })
mongosB.getDB("admin").runCommand({ flushRouterConfig : 1 })
otherCollA.findOne()
otherCollB.findOne()

printjson( mongosA.getDB( "admin" ).runCommand({ shardCollection : "" + collA, key : { _id : 1 } }) )

// Make sure mongosA knows about the collA sharding
mongosA.getDB("admin").runCommand({ flushRouterConfig : 1 })
collA.findOne();

printjson( config.collections.find().toArray() )

sleep( 3000 )

jsTestLog( "Trigger wbl..." )

//collB.findOne()
collB.insert({ goodbye : "world" })
assert.eq( null, collB.getDB().getLastError() )

print( "Inserted..." )

assert.eq( 3, collA.find().itcount() )
assert.eq( 3, collB.find().itcount() )

st.stop()