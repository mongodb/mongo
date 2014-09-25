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
var mongoses = st._mongos

mongoses[0].getDB( "admin" ).createUser({ user: "root", pwd: "pass", roles: ["root"] });

for( var i = 0; i < configs.length; i++ ){
    var confAdmin = configs[i].getDB( "admin" );
    confAdmin.auth( "root", "pass" );
    printjson( confAdmin.runCommand({ getCmdLineOpts : 1 }) )
    assert.eq( confAdmin.runCommand({ getCmdLineOpts : 1 }).parsed.security.keyFile, keyFile )
}

for( var i = 0; i < mongoses.length; i++ ){
    var monsAdmin = mongoses[i].getDB( "admin" );
    monsAdmin.auth( "root", "pass" );
    printjson( monsAdmin.runCommand({ getCmdLineOpts : 1 }) )
    assert.eq( monsAdmin.runCommand({ getCmdLineOpts : 1 }).parsed.security.keyFile, keyFile )
}

var mongos = new Mongo( "localhost:" + st.s0.port )
var coll = mongos.getDB( "test" ).foo;

mongos.getDB( "admin" ).auth( "root", "pass" );
mongos.getDB( "admin" ).runCommand({shardCollection : coll, key : {_id : 1}});

// Create an index so we can find by num later
coll.ensureIndex({ insert : 1 })

// For more logging
// mongos.getDB("admin").runCommand({ setParameter : 1, logLevel : 3 })

print( "INSERT!" )

// Insert a bunch of data
var toInsert = 2000;
var bulk = coll.initializeUnorderedBulkOp();
for( var i = 0; i < toInsert; i++ ){
    bulk.insert({ my : "test", data : "to", insert : i });
}
assert.writeOK(bulk.execute());

print( "UPDATE!" )

// Update a bunch of data
var toUpdate = toInsert;
bulk = coll.initializeUnorderedBulkOp();
for( var i = 0; i < toUpdate; i++ ){
    var id = coll.findOne({ insert : i })._id;
    bulk.find({ insert : i, _id : id }).updateOne({ $inc : { counter : 1 } });
}
assert.writeOK(bulk.execute());

print( "DELETE" )

// Remove a bunch of data
var toDelete = toInsert / 2;
bulk = coll.initializeUnorderedBulkOp();
for( var i = 0; i < toDelete; i++ ){
    bulk.find({ insert : i }).remove();
}
assert.writeOK(bulk.execute());

// Make sure the right amount of data is there
assert.eq( coll.find().count(), toInsert / 2 )

// Finish
st.stop()
