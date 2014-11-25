//
// Tests that mongos validating writes when stale does not DOS config servers
//
// Note that this is *unsafe* with broadcast removes and updates
//

var st = new ShardingTest({ shards : 2, mongos : 3, other : { separateConfig : true,
                                                              shardOptions : { verbose : 2 } } });
st.stopBalancer()

var mongos = st.s0
var staleMongosA = st.s1
var staleMongosB = st.s2

// Additional logging
printjson( mongos.getDB( "admin" ).runCommand({ setParameter : 1, logLevel : 2 }) )
printjson( staleMongosA.getDB( "admin" ).runCommand({ setParameter : 1, logLevel : 2 }) )
printjson( staleMongosB.getDB( "admin" ).runCommand({ setParameter : 1, logLevel : 2 }) )
printjson( st._connections[0].getDB( "admin" ).runCommand({ setParameter : 1, logLevel : 2 }) )
printjson( st._connections[1].getDB( "admin" ).runCommand({ setParameter : 1, logLevel : 2 }) )

var admin = mongos.getDB( "admin" )
var config = mongos.getDB( "config" )
var coll = mongos.getCollection( "foo.bar" )
var staleCollA = staleMongosA.getCollection( coll + "" )
var staleCollB = staleMongosB.getCollection( coll + "" )

printjson( admin.runCommand({ enableSharding : coll.getDB() + "" }) )
coll.ensureIndex({ a : 1 })
printjson( admin.runCommand({ shardCollection : coll + "", key : { a : 1 } }) )

// Let the stale mongos see the collection state
staleCollA.findOne()
staleCollB.findOne()

// Change the collection sharding state
coll.drop()
coll.ensureIndex({ b : 1 })
printjson( admin.runCommand({ shardCollection : coll + "", key : { b : 1 } }) )

// Make sure that we can successfully insert, even though we have stale state
staleCollA.insert({ b : "b" })
assert.eq( null, staleCollA.getDB().getLastError() )

// Make sure we unsuccessfully insert with old info
staleCollB.insert({ a : "a" })
assert.neq( null, staleCollB.getDB().getLastError() )

// Change the collection sharding state
coll.drop()
coll.ensureIndex({ c : 1 })
printjson( admin.runCommand({ shardCollection : coll + "", key : { c : 1 } }) )

// Make sure we can successfully upsert, even though we have stale state
staleCollA.update({ c : "c" }, { c : "c" }, true )
assert.eq( null, staleCollA.getDB().getLastError() )

// Make sure we unsuccessfully upsert with old info
staleCollB.update({ b : "b" }, { b : "b" }, true )
assert.neq( null, staleCollB.getDB().getLastError() )

// Change the collection sharding state
coll.drop()
coll.ensureIndex({ d : 1 })
printjson( admin.runCommand({ shardCollection : coll + "", key : { d : 1 } }) )

// Make sure we can successfully update, even though we have stale state
coll.insert({ d : "d" })
coll.getDB().getLastError();

staleCollA.update({ d : "d" }, { $set : { x : "x" } }, false, false )
assert.eq( null, staleCollA.getDB().getLastError() )
assert.eq( staleCollA.findOne().x, "x" )

// Make sure we unsuccessfully update with old info
staleCollB.update({ c : "c" }, { $set : { x : "y" } }, false, false )
assert.neq( null, staleCollB.getDB().getLastError() )
assert.eq( staleCollB.findOne().x, "x" )

// Change the collection sharding state
coll.drop()
coll.ensureIndex({ e : 1 })
// Deletes need to be across two shards to trigger an error - this is probably an exceptional case
printjson( admin.runCommand({ movePrimary : coll.getDB() + "", to : "shard0000" }) )
printjson( admin.runCommand({ shardCollection : coll + "", key : { e : 1 } }) )
printjson( admin.runCommand({ split : coll + "", middle : { e : 0 } }) )
printjson( admin.runCommand({ moveChunk : coll + "", find : { e : 0 }, to : "shard0001" }) )

// Make sure we can successfully remove, even though we have stale state
coll.insert({ e : "e" })
// Need to make sure the insert makes it to the shard
assert.eq( null, coll.getDB().getLastError() )

staleCollA.remove({ e : "e" }, true)
assert.eq( null, staleCollA.getDB().getLastError() )
assert.eq( null, staleCollA.findOne() )

// Make sure we unsuccessfully remove with old info
staleCollB.remove({ d : "d" }, true )
assert.neq( null, staleCollB.getDB().getLastError() )

st.stop()
