// Tests the dropping of a sharded database SERVER-3471 SERVER-1726

var st = new ShardingTest({ name : jsTestName() })
st.stopBalancer()

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
    
    var key = { _id : 1 }
    st.shardColl( dbA.getCollection( "data" + i ), key )
    st.shardColl( dbB.getCollection( "data" + i ), key )
    st.shardColl( dbC.getCollection( "data" + i ), key )

}

print( "3: drop the non-suffixed db ")

dbA.dropDatabase()


print( "3: ensure only the non-suffixed db was dropped ")

var dbs = mongos.getDBNames()
for( var i = 0; i < dbs.length; i++ ){
    assert.neq( dbs, "" + dbA )
}

assert.eq( 0, config.databases.find({ _id : "" + dbA }).toArray().length )
assert.eq( 1, config.databases.find({ _id : "" + dbB }).toArray().length )
assert.eq( 1, config.databases.find({ _id : "" + dbC }).toArray().length )

assert.eq( numColls, config.collections.find({ _id : RegExp( "^" + dbA + "\\..*" ), dropped : true }).toArray().length )
assert.eq( numColls, config.collections.find({ _id : RegExp( "^" + dbB + "\\..*" ), dropped : false }).toArray().length )
assert.eq( numColls, config.collections.find({ _id : RegExp( "^" + dbC + "\\..*" ), dropped : false }).toArray().length )

for( var i = 0; i < numColls; i++ ){
    
    assert.eq( numDocs / numColls, dbB.getCollection( "data" + (i % numColls) ).find().itcount() )
    assert.eq( numDocs / numColls, dbC.getCollection( "data" + (i % numColls) ).find().itcount() ) 
    
}

// Finish
st.stop()
