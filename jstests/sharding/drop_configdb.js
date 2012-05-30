var shardA = startMongodEmpty("--shardsvr", "--port", 30001, "--dbpath", "/data/shardA", "--nopreallocj");
var shardB = startMongodEmpty("--shardsvr", "--port", 30002, "--dbpath", "/data/shardB", "--nopreallocj");
var configsvr = startMongodEmpty("--configsvr", "--port", 29999, "--dbpath", "/data/configC", "--nopreallocj");

var mongos = startMongos({ port : 30000, configdb : "localhost:29999" })

var admin = mongos.getDB("admin")

admin.runCommand({ addshard : "localhost:30001" })
admin.runCommand({ addshard : "localhost:30002" })

var config = configsvr.getDB( "config" )

// Try to drop config db via configsvr

print ( "1: Try to drop config database via configsvr" )
config.dropDatabase()

print ( "2: Ensure it wasn't dropped" )
assert.eq( 1, config.databases.find({ _id : "admin", partitioned : false, primary : "config"}).toArray().length )


// Try to drop config db via mongos

var config = mongos.getDB( "config" )

print ( "1: Try to drop config database via mongos" )
config.dropDatabase()

print ( "2: Ensure it wasn't dropped" )
assert.eq( 1, config.databases.find({ _id : "admin", partitioned : false, primary : "config"}).toArray().length )


// Finish
stopMongod( 30000 );
stopMongod( 29999 );
stopMongod( 30001 );
stopMongod( 30002 );
