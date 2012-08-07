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
    var myjson = coll.getDB().getLastErrorObj();
    // for example -- err : "socket exception [SEND_ERROR] for 127.0.0.1:30001"
    //  or            err : "socket exception [CONNECT_ERROR] for localhost:30001"
    var errorStringParts = myjson.err.split(" ");
    if ( errorStringParts.length < 2 ||
         errorStringParts[0] != "socket" ||
         errorStringParts[1] != "exception" ) {
        gle_state = 1;
        print( "Unit test failure -- received response from getLastError:" );
        printjson( myjson );
    }
}
catch( e ){
    printjson( e )
}
assert( !gle_state );
jsTestLog( "Inserting into positive shard..." )

coll.insert({ _id : 1 })
print( "GLE start" )
printjson( coll.getDB().getLastError() )
print( "test ending" )
st.stop()

