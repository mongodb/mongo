// Tests sharding with a key file

myTestName = "sharding_with_keyfile"

keyFile = "jstests/sharding/" + myTestName + ".key";

run( "chmod" , "600" , keyFile );

var st = new ShardingTest({ name : myTestName ,
                            shards : 2,
                            mongos : 1,
                            keyFile : keyFile })

// Make sure all our instances got the key
var configs = st._configServers
var shards = st._connections
var mongoses = st._mongos

for( var i = 0; i < configs.length; i++ ){
    printjson( new Mongo( "localhost:" + configs[i].port ).getDB("admin").runCommand({ getCmdLineOpts : 1 }) )
    assert.eq( new Mongo( "localhost:" + configs[i].port ).getDB("admin").runCommand({ getCmdLineOpts : 1 }).parsed.keyFile, keyFile )
}

for( var i = 0; i < shards.length; i++ ){
    printjson( new Mongo( "localhost:" + shards[i].port ).getDB("admin").runCommand({ getCmdLineOpts : 1 }) )
    assert.eq( new Mongo( "localhost:" + shards[i].port ).getDB("admin").runCommand({ getCmdLineOpts : 1 }).parsed.keyFile, keyFile )
}
    
for( var i = 0; i < mongoses.length; i++ ){
    printjson( new Mongo( "localhost:" + mongoses[i].port ).getDB("admin").runCommand({ getCmdLineOpts : 1 }) )
    assert.eq( new Mongo( "localhost:" + mongoses[i].port ).getDB("admin").runCommand({ getCmdLineOpts : 1 }).parsed.keyFile, keyFile )
}

var mongos = new Mongo( "localhost:" + st.s0.port )
var coll = mongos.getCollection( "test.foo" )

st.shardColl( coll, { _id : 1 }, false )

// Create an index so we can find by num later
coll.ensureIndex({ insert : 1 })

// For more logging
// mongos.getDB("admin").runCommand({ setParameter : 1, logLevel : 3 })

print( "INSERT!" )

// Insert a bunch of data
var toInsert = 2000
for( var i = 0; i < toInsert; i++ ){
    coll.insert({ my : "test", data : "to", insert : i })
}

assert.eq( coll.getDB().getLastError(), null )

print( "UPDATE!" )

// Update a bunch of data
var toUpdate = toInsert
for( var i = 0; i < toUpdate; i++ ){
    var id = coll.findOne({ insert : i })._id
    coll.update({ insert : i, _id : id }, { $inc : { counter : 1 } })
}

assert.eq( coll.getDB().getLastError(), null )

print( "DELETE" )

// Remove a bunch of data
var toDelete = toInsert / 2
for( var i = 0; i < toDelete; i++ ){
    coll.remove({ insert : i })
}

assert.eq( coll.getDB().getLastError(), null )

// Make sure the right amount of data is there
assert.eq( coll.find().count(), toInsert / 2 )

// Finish
st.stop()
