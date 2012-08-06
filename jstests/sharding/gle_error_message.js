//
// Tests whether sharded GLE fails sanely and correctly reports failures.  
//

jsTest.log( "Starting sharded cluster..." )

var st = new ShardingTest({ shards : 3,
                            mongos : 2,
                            verbose : 3,
                            other : { separateConfig : true } })

st.stopBalancer()

var mongos = st.s0
var admin = mongos.getDB( "admin" )
var config = mongos.getDB( "config" )
var coll = mongos.getCollection( jsTestName() + ".coll" )
var shards = config.shards.find().toArray()

jsTest.log( "Enabling sharding..." )

printjson( admin.runCommand({ enableSharding : "" + coll.getDB() }) )
printjson( admin.runCommand({ movePrimary : "" + coll.getDB(), to : shards[0]._id }) )
printjson( admin.runCommand({ shardCollection : "" + coll, key : { _id : 1 } }) )
printjson( admin.runCommand({ split : "" + coll, middle : { _id : 0 } }) )
printjson( admin.runCommand({ moveChunk : "" + coll, find : { _id : 0 }, to : shards[1]._id }) )

st.printShardingStatus()



jsTest.log( "Testing GLE...")

// insert to two diff shards
coll.insert({ _id : -1, hello : "world" })
coll.insert({ _id : 1, hello : "world" })

jsTest.log( "GLE : " + tojson( coll.getDB().getLastErrorObj() ) )



jsTest.log( "Testing GLE when writeback host goes down..." )

// insert to two diff shards
coll.insert({ _id : -2, hello : "world" })
coll.insert({ _id : 2, hello : "world" })

MongoRunner.stopMongod( st.shard0 )

jsTest.log( "GLE : " + tojson( coll.getDB().getLastErrorObj() ) )

st.shard0 = MongoRunner.runMongod( st.shard0 )



jsTest.log( "Testing GLE when main host goes down..." )

// insert to two diff shards
coll.insert({ _id : -3, hello : "world" })
coll.insert({ _id : 3, hello : "world" })

MongoRunner.stopMongod( st.shard1 )

try{ 
    jsTest.log( "Calling GLE! " ) 
    coll.getDB().getLastErrorObj()
    assert( false )
}
catch( e ){
    jsTest.log( "GLE : " + e )
    
    // Stupid string exceptions
    assert( /could not get last error/.test( e + "") )
}

st.shard1 = MongoRunner.runMongod( st.shard1 )



jsTest.log( "Testing multi GLE for multi-host writes..." )

coll.update({ hello : "world" }, { $set : { goodbye : "world" } }, false, true)

jsTest.log( "GLE : " + tojson( coll.getDB().getLastErrorObj() ) )

jsTest.log( "Testing multi GLE when host goes down..." )

// insert to two diff shards
coll.update({ hello : "world" }, { $set : { goodbye : "world" } }, false, true)

MongoRunner.stopMongod( st.shard0 )

try{ 
    jsTest.log( "Calling GLE! " ) 
    coll.getDB().getLastErrorObj()
    assert( false )
}
catch( e ){
    jsTest.log( "GLE : " + e )
    
    // Stupid string exceptions
    assert( /could not get last error/.test( e + "") )
}

st.shard0 = MongoRunner.runMongod( st.shard0 )



jsTest.log( "Testing stale version GLE when host goes down..." )

var staleColl = st.s1.getCollection( coll + "" )
staleColl.findOne()

printjson( admin.runCommand({ moveChunk : "" + coll, find : { _id : 0 }, to : shards[2]._id }) )

MongoRunner.stopMongod( st.shard2 )

jsTest.log( "Sending stale write..." )

staleColl.insert({ _id : 4, hello : "world" })

assert.neq( null, staleColl.getDB().getLastError() )


jsTest.log( "Done!" )

st.stop()