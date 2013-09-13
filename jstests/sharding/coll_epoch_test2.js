// Tests that resharding a collection is detected correctly by all operation types
// 
// The idea here is that a collection may be resharded / unsharded at any point, and any type of
// operation on a mongos may be active when it happens.  All operations should handle gracefully.
//

var st = new ShardingTest({ shards : 2, mongos : 5, verbose : 1, separateConfig : 1  })
// Stop balancer, it'll interfere
st.stopBalancer()

// Use separate mongos for reading, updating, inserting, removing data
var readMongos = st.s1
var updateMongos = st.s2
var insertMongos = st.s3
var removeMongos = st.s4

var config = st.s.getDB( "config" )
var admin = st.s.getDB( "admin" )
var coll = st.s.getCollection( "foo.bar" )

insertMongos.getDB( "admin" ).runCommand({ setParameter : 1, traceExceptions : true })

var shards = {}
config.shards.find().forEach( function( doc ){ 
    shards[ doc._id ] = new Mongo( doc.host )
})

//
// Set up a sharded collection
//

jsTest.log( "Enabling sharding for the first time..." )

admin.runCommand({ enableSharding : coll.getDB() + "" })
admin.runCommand({ shardCollection : coll  + "", key : { _id : 1 } })
    
coll.insert({ hello : "world" }) 
assert.eq( null, coll.getDB().getLastError() )

jsTest.log( "Sharding collection across multiple shards..." )
    
var getOtherShard = function( shard ){
    for( id in shards ){
        if( id != shard ) return id
    }
}
        
printjson( admin.runCommand({ split : coll + "", middle : { _id : 0 } }) )
printjson( admin.runCommand({ moveChunk : coll + "", find : { _id : 0 },
                              to : getOtherShard( config.databases.findOne({ _id : coll.getDB() + "" }).primary ) }) )

st.printShardingStatus()

//
// Force all mongoses to load the current status of the cluster
//

jsTest.log( "Loading this status in all mongoses..." )

for( var i = 0; i < st._mongos.length; i++ ){
    printjson( st._mongos[i].getDB( "admin" ).runCommand({ flushRouterConfig : 1 }) )
    assert.neq( null, st._mongos[i].getCollection( coll + "" ).findOne() )
}

//
// Drop and recreate a new sharded collection in the same namespace, where the shard and collection
// versions are the same, but the split is at a different point.
//

jsTest.log( "Rebuilding sharded collection with different split..." )

coll.drop()

var droppedCollDoc = config.collections.findOne({ _id: coll.getFullName() });
assert(droppedCollDoc != null);
assert.eq(true, droppedCollDoc.dropped);
assert(droppedCollDoc.lastmodEpoch != null);
assert(droppedCollDoc.lastmodEpoch.equals(new ObjectId("000000000000000000000000")),
       "epoch not zero: " + droppedCollDoc.lastmodEpoch);

admin.runCommand({ enableSharding : coll.getDB() + "" })
admin.runCommand({ shardCollection : coll  + "", key : { _id : 1 } })

for( var i = 0; i < 100; i++ ) coll.insert({ _id : i })
assert.eq( null, coll.getDB().getLastError() )

printjson( admin.runCommand({ split : coll + "", middle : { _id : 200 } }) )
printjson( admin.runCommand({ moveChunk : coll + "", find : { _id : 200 },
                              to : getOtherShard( config.databases.findOne({ _id : coll.getDB() + "" }).primary ) }) )

//
// Make sure all operations on mongoses aren't tricked by the change
//                              
                              
jsTest.log( "Checking other mongoses for detection of change..." )

jsTest.log( "Checking find..." )
// Ensure that finding an element works when resharding
assert.neq( null, readMongos.getCollection( coll + "" ).findOne({ _id : 1 }) )

jsTest.log( "Checking update...")
// Ensure that updating an element finds the right location
updateMongos.getCollection( coll + "" ).update({ _id : 1 }, { $set : { updated : true } })
assert.eq( null, updateMongos.getDB( coll.getDB() + "" ).getLastError() )
assert.neq( null, coll.findOne({ updated : true }) )

jsTest.log( "Checking insert..." )
// Ensure that inserting an element finds the right shard
insertMongos.getCollection( coll + "" ).insert({ _id : 101 })
assert.eq( null, insertMongos.getDB( coll.getDB() + "" ).getLastError() )
assert.neq( null, coll.findOne({ _id : 101 }) )

jsTest.log( "Checking remove..." )
// Ensure that removing an element finds the right shard, verified by the mongos doing the sharding
removeMongos.getCollection( coll + "" ).remove({ _id : 2 })
assert.eq( null, removeMongos.getDB( coll.getDB() + "" ).getLastError() )
assert.eq( null, coll.findOne({ _id : 2 }) )

coll.drop()

jsTest.log( "Done!" )

st.stop()
