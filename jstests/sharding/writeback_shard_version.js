// Tests whether a newly sharded collection can be handled by the wbl

jsTestLog( "Starting sharded cluster..." )

// Need to start as a replica set here, just because there's no other way to trigger separate configs,
// See SERVER-4222
var st = new ShardingTest( { shards : 1, mongos : 2, verbose : 2, other : { rs : true } } )

st.setBalancer( false )

var mongosA = st.s0
var mongosB = st.s1

jsTestLog( "Adding new collections...")

var collA = mongosA.getCollection( jsTestName() + ".coll" )
collA.insert({ hello : "world" })
assert.eq( null, collA.getDB().getLastError() )

var collB = mongosB.getCollection( "" + collA )
collB.insert({ hello : "world" })
assert.eq( null, collB.getDB().getLastError() )

jsTestLog( "Enabling sharding..." )

printjson( mongosA.getDB( "admin" ).runCommand({ enableSharding : "" + collA.getDB() }) )
printjson( mongosA.getDB( "admin" ).runCommand({ shardCollection : "" + collA, key : { _id : 1 } }) )

// MongoD doesn't know about the config shard version *until* MongoS tells it
collA.findOne()

jsTestLog( "Trigger wbl..." )

collB.insert({ goodbye : "world" })
assert.eq( null, collB.getDB().getLastError() )

print( "Inserted..." )

assert.eq( 3, collA.find().itcount() )
assert.eq( 3, collB.find().itcount() )

st.stop()