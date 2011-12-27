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
try{
    printjson( coll.getDB().getLastError() )
}
catch( e ){
    printjson( e )
}

jsTestLog( "Inserting into positive shard..." )

coll.insert({ _id : 1 })
print( "GLE start" )
printjson( coll.getDB().getLastError() )

st.stop()

