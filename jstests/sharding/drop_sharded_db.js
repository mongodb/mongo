// Tests the dropping of a sharded database SERVER-3471

var st = new ShardingTest( testName = "dropShardedDb",
                           numShards = 2,
                           verboseLevel = 0,
                           numMongos = 1 )

var mongos = st.s0
var config = mongos.getDB( "config" )

var dbName = "buy"
var dbA = mongos.getDB( dbName )
var dbB = mongos.getDB( dbName + "_201107" )
var dbC = mongos.getDB( dbName + "_201108" )

print( "1: insert some data and colls into all dbs" )

var numDocs = 3000;
var numColls = 10;
for( var i = 0; i < numDocs; i++ ){
    dbA.getCollection( "data" + (i % numColls) ).insert({ _id : i })
    dbB.getCollection( "data" + (i % numColls) ).insert({ _id : i })
    dbC.getCollection( "data" + (i % numColls) ).insert({ _id : i })
}

print( "2: shard the colls ")

for( var i = 0; i < numColls; i++ ){
    
    var splitAt = { _id : numDocs / 2 }
    st.shardGo( dbA.getCollection( "data" + i ), splitAt )
    st.shardGo( dbB.getCollection( "data" + i ), splitAt )
    st.shardGo( dbC.getCollection( "data" + i ), splitAt )

}

print( "3: drop the non-suffixed db ")

dbA.drop()

/*
print( "3: ensure only the non-suffixed db was dropped ")

var dbs = mongos.getDBs()
for( var i = 0; i < dbs.length; i++ ){
    assert.ne( dbs, "" + dbA )
}

assert.eq( 0, config.databases.find({ _id : "" + dbA }).toArray().length )
assert.eq( 1, config.databases.find({ _id : "" + dbB }).toArray().length )
assert.eq( 1, config.databases.find({ _id : "" + dbC }).toArray().length )

assert.eq( 0, config.collections.find({ db : "" + dbA }).toArray().length )
assert.eq( numColls, config.collections.find({ db : "" + dbB }).toArray().length )
assert.eq( numColls, config.collections.find({ db : "" + dbC }).toArray().length )

for( var i = 0; i < numColls; i++ ){
    
    assert.eq( numDocs / numColls, dbB.getCollection( "data" + (i % numColls) ).find().itcount() )
    assert.eq( numDocs / numColls, dbC.getCollection( "data" + (i % numColls) ).find().itcount() ) 
    
}



// Finish
st.stop()
*/