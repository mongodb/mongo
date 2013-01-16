// Test for SERVER-3763

var st = new ShardingTest({ shards : 2 })

var coll = st.s.getCollection( jsTestName() + ".coll" )
coll.drop()

// Shard collection over two shards
st.shardColl( coll, { _id : 1 }, { _id : 0 } )

var negShard = st.getShard( coll, { _id : -1 }, true )
var posShard = st.getShard( coll, { _id : 1 }, true )

jsTestLog( "Sharding status..." )

st.printShardingStatus()

jsTestLog( "Shutting down negative shard..." )

MongoRunner.stopMongod( negShard )

jsTestLog( "Inserting into negative shard..." )

coll.insert({ _id : -1 })
print( "GLE start" )
var gle_state = 0;
try{
    var err = coll.getDB().getLastError();
    
    // for example -- err : "socket exception [SEND_ERROR] for 127.0.0.1:30001"
    //  or            err : "socket exception [CONNECT_ERROR] for localhost:30001"
    
    if (err && !/socket exception/.test(err)) {
        gle_state = 1;
        print( "Test failure -- received response from getLastError:" + err );
    }
    else if(!err) {
        gle_state = 1;
        print("Test failure -- no response from getLastError.");
    }
    else {
        print("Normal socket error detected: " + err);
    }
}
catch( e ){
    print("Error detected when calling GLE, this is normal:")
    printjson( e )
}
assert( !gle_state );
jsTestLog( "Inserting into positive shard..." )

coll.insert({ _id : 1 })
print( "GLE start" )
printjson( coll.getDB().getLastError() )
print( "test ending" )
st.stop()

