// Tests various cases of dropping and recreating collections in the same namespace with multiple mongoses

var st = new ShardingTest({ shards : 3, mongos : 3, verbose : 1, separateConfig : 1  })
// Stop balancer, it'll interfere
st.stopBalancer()

// Use separate mongoses for admin, inserting data, and validating results, so no
// single-mongos tricks will work
var insertMongos = st.s2
var staleMongos = st.s1

var config = st.s.getDB( "config" )
var admin = st.s.getDB( "admin" )
var coll = st.s.getCollection( "foo.bar" )

insertMongos.getDB( "admin" ).runCommand({ setParameter : 1, traceExceptions : true })

var shards = {}
config.shards.find().forEach( function( doc ){ 
    shards[ doc._id ] = new Mongo( doc.host )
})

//
// Test that inserts and queries go to the correct shard even when the collection has been sharded
// in the background
//

jsTest.log( "Enabling sharding for the first time..." )

admin.runCommand({ enableSharding : coll.getDB() + "" })
admin.runCommand({ shardCollection : coll  + "", key : { _id : 1 } })

for( var i = 0; i < 100; i++ )
    insertMongos.getCollection( coll + "" ).insert({ _id : i, test : "a" })
assert.eq( null, insertMongos.getDB( coll.getDB() + "" ).getLastError() )    
assert.eq( 100, staleMongos.getCollection( coll + "" ).find({ test : "a" }).itcount() )

coll.drop()

//
// Test that inserts and queries go to the correct shard even when the collection has been 
// re-sharded in the background
//

jsTest.log( "Re-enabling sharding with a different key..." )

admin.runCommand({ enableSharding : coll.getDB() + "" })
coll.ensureIndex({ notId : 1 })
admin.runCommand({ shardCollection : coll  + "", key : { notId : 1 } })

for( var i = 0; i < 100; i++ )
    insertMongos.getCollection( coll + "" ).insert({ notId : i, test : "b" })
assert.eq( null, insertMongos.getDB( coll.getDB() + "" ).getLastError() )
assert.eq( 100, staleMongos.getCollection( coll + "" ).find({ test : "b" }).itcount() )
assert.eq( 0, staleMongos.getCollection( coll + "" ).find({ test : { $in : [ "a" ] } }).itcount() )

coll.drop()

//
// Test that inserts and queries go to the correct shard even when the collection has been 
// unsharded and moved to a different primary
//

jsTest.log( "Re-creating unsharded collection from a sharded collection on different primary..." )

var getOtherShard = function( shard ){
    for( id in shards ){
        if( id != shard ) return id
    }
}

admin.runCommand({ movePrimary : coll.getDB() + "", 
                   to : getOtherShard( config.databases.findOne({ _id : coll.getDB() + "" }).primary ) })

jsTest.log( "moved primary..." )
                   
for( var i = 0; i < 100; i++ )
    insertMongos.getCollection( coll + "" ).insert({ test : "c" })
assert.eq( null, insertMongos.getDB( coll.getDB() + "" ).getLastError() )

jsTest.log( "waited for gle..." )

assert.eq( 100, staleMongos.getCollection( coll + "" ).find({ test : "c" }).itcount() )
assert.eq( 0, staleMongos.getCollection( coll + "" ).find({ test : { $in : [ "a", "b" ] } }).itcount() )

coll.drop()

//
// Test that inserts and queries go to correct shard even when the collection has been unsharded, 
// resharded, and moved to a different primary
//

jsTest.log( "Re-creating sharded collection with different primary..." )

admin.runCommand({ enableSharding : coll.getDB() + "" })
admin.runCommand({ movePrimary : coll.getDB() + "", 
                   to : getOtherShard( config.databases.findOne({ _id : coll.getDB() + "" }).primary ) })
admin.runCommand({ shardCollection : coll  + "", key : { _id : 1 } })

for( var i = 0; i < 100; i++ )
    insertMongos.getCollection( coll + "" ).insert({ test : "d" })
assert.eq( null, insertMongos.getDB( coll.getDB() + "" ).getLastError() )
assert.eq( 100, staleMongos.getCollection( coll + "" ).find({ test : "d" }).itcount() )
assert.eq( 0, staleMongos.getCollection( coll + "" ).find({ test : { $in : [ "a", "b", "c" ] } }).itcount() )

coll.drop()

jsTest.log( "Done!" )

st.stop()