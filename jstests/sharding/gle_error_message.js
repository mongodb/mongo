//
// Tests whether sharded GLE fails sanely and correctly reports failures.  
//

function waitForWrite(shardIndex, query, count) {
    var searchText = tojson(query) + " on shard " + shardIndex;
    jsTest.log( "Waiting for " + count + " document(s) with " + searchText );
    var shardDB = connect(shards[shardIndex].host + "/" + jsTestName());
    assert.soon( function() { return shardDB.coll.find( query ).count() == count; },
                 "Failed to find document with " + searchText,
                 /* timeout */ 10 * 1000,
                 /*interval*/ 10 );
}

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

waitForWrite(0, {_id: -1}, 1);
waitForWrite(1, {_id:  1}, 1);

jsTest.log( "GLE : " + tojson( coll.getDB().getLastErrorObj() ) )



jsTest.log( "Testing GLE when writeback host goes down..." )

// insert to two diff shards
coll.insert({ _id : -2, hello : "world" })
coll.insert({ _id : 2, hello : "world" })

waitForWrite(0, {_id: -2}, 1);
waitForWrite(1, {_id:  2}, 1);

MongoRunner.stopMongod( st.shard0 )

jsTest.log( "GLE : " + tojson( coll.getDB().getLastErrorObj() ) )

st.shard0 = MongoRunner.runMongod( st.shard0 )



jsTest.log( "Testing GLE when main host goes down..." )

// insert to two diff shards
coll.insert({ _id : -3, hello : "world" })
coll.insert({ _id : 3, hello : "world" })

waitForWrite(0, {_id: -3}, 1);
waitForWrite(1, {_id:  3}, 1);

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

waitForWrite(0, {goodbye: "world"}, 3);
waitForWrite(1, {goodbye: "world"}, 3);

jsTest.log( "GLE : " + tojson( coll.getDB().getLastErrorObj() ) )

jsTest.log( "Testing multi GLE when host goes down..." )

// insert to two diff shards
coll.update({ hello : "world" }, { $set : { goodnight : "moon" } }, false, true)

waitForWrite(0, {goodnight: "moon"}, 3);
waitForWrite(1, {goodnight: "moon"}, 3);

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

printjson( admin.runCommand({ connPoolStats : true }) );
//printjson( admin.runCommand({ connPoolSync : true }) );
assert( admin.runCommand({ moveChunk : "" + coll, find : { _id : 0 }, to : shards[2]._id }).ok );

waitForWrite(2, {goodnight: "moon"}, 3);

MongoRunner.stopMongod( st.shard2 )

jsTest.log( "Sending stale write..." )

staleColl.insert({ _id : 4, hello : "world" })

assert.neq( null, staleColl.getDB().getLastError() )


jsTest.log( "Done!" )

st.stop()
