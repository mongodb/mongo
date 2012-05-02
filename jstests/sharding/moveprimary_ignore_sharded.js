// Checks that movePrimary doesn't move collections detected as sharded when it begins moving

var st = new ShardingTest({ shards : 2, mongos : 2, verbose : 1 })

var mongosA = st.s0
var mongosB = st.s1

st.shard0.getDB( "admin" ).runCommand({ setParameter : 1, logLevel : 2 })
st.shard1.getDB( "admin" ).runCommand({ setParameter : 1, logLevel : 2 })

var adminA = mongosA.getDB( "admin" )
var adminB = mongosB.getDB( "admin" )

var configA = mongosA.getDB( "config" )

// Setup three collections per-db
// 0 : not sharded
// 1 : sharded
// 2 : sharded but not seen as sharded by mongosB

var collsFooA = []
var collsFooB = []
var collsBarA = []
var collsBarB = []

for( var i = 0; i < 3; i++ ){
    
    collsFooA.push( mongosA.getCollection( "foo.coll" + i ) )
    collsFooB.push( mongosB.getCollection( "foo.coll" + i ) )
    collsBarA.push( mongosA.getCollection( "bar.coll" + i ) )
    collsBarB.push( mongosB.getCollection( "bar.coll" + i ) )
    
    collsFooA[i].insert({ hello : "world" })
    assert.eq( null, collsFooA[i].getDB().getLastError() )
    collsBarA[i].insert({ hello : "world" })
    assert.eq( null, collsBarA[i].getDB().getLastError() )
    
}

// Enable sharding
printjson( adminA.runCommand({ enableSharding : collsFooA[0].getDB() + "" }) )
printjson( adminA.runCommand({ enableSharding : collsBarA[0].getDB() + "" }) )

printjson( adminA.runCommand({ shardCollection : collsFooA[1] + "", key : { _id : 1 } }) )
printjson( adminA.runCommand({ shardCollection : collsFooA[2] + "", key : { _id : 1 } }) )
printjson( adminA.runCommand({ shardCollection : collsBarA[1] + "", key : { _id : 1 } }) )
printjson( adminA.runCommand({ shardCollection : collsBarA[2] + "", key : { _id : 1 } }) )

// All collections are now on primary shard
var fooPrimaryShard = configA.databases.findOne({ _id : collsFooA[0].getDB() + "" }).primary
var barPrimaryShard = configA.databases.findOne({ _id : collsBarA[0].getDB() + "" }).primary

var shards = configA.shards.find().toArray()
var fooPrimaryShard = fooPrimaryShard == shards[0]._id ? shards[0]  : shards[1]
var fooOtherShard = fooPrimaryShard._id == shards[0]._id ? shards[1]  : shards[0]
var barPrimaryShard = barPrimaryShard == shards[0]._id  ? shards[0] : shards[1] 
var barOtherShard = barPrimaryShard._id == shards[0]._id  ? shards[1] : shards[0] 

jsTest.log( "Setup collections for moveprimary test..." )
st.printShardingStatus()

jsTest.log( "Running movePrimary for foo through mongosA ..." )

// MongosA should already know about all the collection states

printjson( adminA.runCommand({ movePrimary : collsFooA[0].getDB() + "", to : fooOtherShard._id }) )

jsTest.log( "Run!" )

// All collections still correctly sharded / unsharded
assert.neq( null, collsFooA[0].findOne() )
assert.neq( null, collsFooA[1].findOne() )
assert.neq( null, collsFooA[2].findOne() )

assert.neq( null, collsFooB[0].findOne() )
assert.neq( null, collsFooB[1].findOne() )
assert.neq( null, collsFooB[2].findOne() )

// All indexes sane
assert.eq( 2, new Mongo( fooPrimaryShard.host ).getCollection( collsFooA[0].getDB() + ".system.indexes" ).find().count() )
assert.eq( 1, new Mongo( fooOtherShard.host ).getCollection( collsFooA[0].getDB() + ".system.indexes" ).find().count() )

jsTest.log( "Running movePrimary for bar through mongosB ..." )

// Make mongosB detect the first two collections in bar, but not third, to make sure 
// we refresh our config state before doing dangerous things
collsBarB[0].findOne()
collsBarB[1].findOne()

printjson( adminB.runCommand({ movePrimary : collsBarA[0].getDB() + "", to : barOtherShard._id }) )

jsTest.log( "Run!" )

// All collections still correctly sharded / unsharded
assert.neq( null, collsBarA[0].findOne() )
assert.neq( null, collsBarA[1].findOne() )
assert.neq( null, collsBarA[2].findOne() )

assert.neq( null, collsBarB[0].findOne() )
assert.neq( null, collsBarB[1].findOne() )
assert.neq( null, collsBarB[2].findOne() )

// All indexes sane
assert.eq( 2, new Mongo( barPrimaryShard.host ).getCollection( collsBarA[0].getDB() + ".system.indexes" ).find().count() )
assert.eq( 1, new Mongo( barOtherShard.host ).getCollection( collsBarA[0].getDB() + ".system.indexes" ).find().count() )

st.stop()




