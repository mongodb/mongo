//Test that oplog is created and populated on the config server by default

print( "Start config_oplog.js" );

var test = new ShardingTest( {mongos : 1, shards : 1, config : 1, other: {separateConfig:true}} );
var oplogCol = test.config0.getDB( "local" ).getCollection( "oplog.$main" );
var stats = oplogCol.stats( );

assert.eq( true, stats.capped );
assert.eq( 5 * 1024 * 1024, stats.storageSize );

test.admin.runCommand( {enableSharding : "test"} );

//make sure enableSharding was recorded in oplog
var oplogEntry = oplogCol.find( {ns : "config.databases"} ).sort( {$natural : -1} ).limit( 1 ).next();

assert.eq( "test", oplogEntry.o._id );
assert.eq( true, oplogEntry.o.partitioned );

test.stop();

var conn = startMongodTest( 30001, "config_oplog", false, { configsvr : "", oplogSize : 2 } );
stats = conn.getDB( "local" ).getCollection( "oplog.$main" ).stats();

assert.eq( true, stats.capped );
assert.eq( 2 * 1024 * 1024, stats.storageSize );

stopMongoProgram( 30001 );

print("END config_oplog.js");