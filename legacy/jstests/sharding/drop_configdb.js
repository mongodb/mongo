// Test that dropping the config database is completely disabled via
// mongos and via mongod, if started with --configsvr
var st = new ShardingTest({ shards : 2, config : 1, other : {separateConfig : true}});
var mongos = st.s;
var config = st._configServers[0].getDB('config');

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

st.stop();