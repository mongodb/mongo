// Tests whether a reset sharding version triggers errors

jsTest.log( "Starting sharded cluster..." )

var st = new ShardingTest( { shards : 1, mongos : 2, verbose : 2, separateConfig : 1  } )

st.stopBalancer()

var mongosA = st.s0
var mongosB = st.s1

jsTest.log( "Adding new collections...")

var collA = mongosA.getCollection( jsTestName() + ".coll" )
collA.insert({ hello : "world" })
assert.eq( null, collA.getDB().getLastError() )

var collB = mongosB.getCollection( "" + collA )
collB.insert({ hello : "world" })
assert.eq( null, collB.getDB().getLastError() )

jsTest.log( "Enabling sharding..." )

printjson( mongosA.getDB( "admin" ).runCommand({ enableSharding : "" + collA.getDB() }) )
printjson( mongosA.getDB( "admin" ).runCommand({ shardCollection : "" + collA, key : { _id : 1 } }) )

// MongoD doesn't know about the config shard version *until* MongoS tells it
collA.findOne()

jsTest.log( "Trigger wbl..." )

collB.insert({ goodbye : "world" })
assert.eq( null, collB.getDB().getLastError() )

print( "Inserted..." )

assert.eq( 3, collA.find().itcount() )
assert.eq( 3, collB.find().itcount() )

st.stop()