//
// Tests cleanup of orphaned data via the orphaned data cleanup command
//

var options = { separateConfig : true, shardOptions : { verbose : 2 } };

var st = new ShardingTest({ shards : 2, mongos : 2, other : options });
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB( "admin" );
var shards = mongos.getCollection( "config.shards" ).find().toArray();
var coll = mongos.getCollection( "foo.bar" );

assert( admin.runCommand({ enableSharding : coll.getDB() + "" }).ok );
printjson( admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }) );
assert( admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : 0 } }).ok );
assert( admin.runCommand({ moveChunk : coll + "", 
                           find : { _id : 0 }, 
                           to : shards[1]._id,
                           _waitForDelete : true }).ok );

st.printShardingStatus();

jsTest.log( "Inserting some regular docs..." );

for ( var i = -50; i < 50; i++ ) coll.insert({ _id : i });
assert.eq( null, coll.getDB().getLastError() );

// Half of the data is on each shard

jsTest.log( "Inserting some orphaned docs..." );

var shard0Coll = st.shard0.getCollection( coll + "" );
shard0Coll.insert({ _id : 10 });
assert.eq( null, shard0Coll.getDB().getLastError() );

assert.neq( 50, shard0Coll.count() );
assert.eq( 100, coll.find().itcount() );

jsTest.log( "Cleaning up orphaned data..." );

var shard0Admin = st.shard0.getDB( "admin" );
var result = shard0Admin.runCommand({ cleanupOrphaned : coll + "" });
while ( result.ok && result.stoppedAtKey ) {
    printjson( result );
    result = shard0Admin.runCommand({ cleanupOrphaned : coll + "",
                                      startingFromKey : result.stoppedAtKey });
}

printjson( result );
assert( result.ok );
assert.eq( 50, shard0Coll.count() );
assert.eq( 100, coll.find().itcount() );

jsTest.log( "Moving half the data out again (making a hole)..." );

assert( admin.runCommand({ split : coll + "", middle : { _id : -35 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : -10 } }).ok );
// Make sure we wait for the deletion here, otherwise later cleanup could fail
assert( admin.runCommand({ moveChunk : coll + "", 
                           find : { _id : -35 }, 
                           to : shards[1]._id,
                           _waitForDelete : true }).ok );

// 1/4 the data is on the first shard

jsTest.log( "Inserting some more orphaned docs..." );

var shard0Coll = st.shard0.getCollection( coll + "" );
shard0Coll.insert({ _id : -36 });
shard0Coll.insert({ _id : -10 });
shard0Coll.insert({ _id : 0 });
shard0Coll.insert({ _id : 10 });
assert.eq( null, shard0Coll.getDB().getLastError() );

assert.neq( 25, shard0Coll.count() );
assert.eq( 100, coll.find().itcount() );

jsTest.log( "Cleaning up more orphaned data..." );

var shard0Admin = st.shard0.getDB( "admin" );
var result = shard0Admin.runCommand({ cleanupOrphaned : coll + "" });
while ( result.ok && result.stoppedAtKey ) {
    printjson( result );
    result = shard0Admin.runCommand({ cleanupOrphaned : coll + "",
                                      startingFromKey : result.stoppedAtKey });
}

printjson( result );
assert( result.ok );
assert.eq( 25, shard0Coll.count() );
assert.eq( 100, coll.find().itcount() );

jsTest.log( "DONE!" );

st.stop();
