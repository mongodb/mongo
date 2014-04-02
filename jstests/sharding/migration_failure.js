//
// Tests that migration failures before and after commit correctly roll back 
// when possible
//

var options = { separateConfig : true };

var st = new ShardingTest({ shards : 2, mongos : 1, other : options });
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB( "admin" );
var shards = mongos.getCollection( "config.shards" ).find().toArray();
var coll = mongos.getCollection( "foo.bar" );

assert( admin.runCommand({ enableSharding : coll.getDB() + "" }).ok );
printjson( admin.runCommand({ movePrimary : coll.getDB() + "", to : shards[0]._id }) );
assert( admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : 0 } }).ok );

st.printShardingStatus();

jsTest.log("Testing failed migrations...");

assert.commandWorked(
    st.shard0.getDB("admin").runCommand({
        configureFailPoint : 'failMigrationConfigWritePrepare', mode : 'alwaysOn' }));

var version = st.shard0.getDB("admin").runCommand({ getShardVersion : coll.toString() });

assert.commandFailed( admin.runCommand({ moveChunk : coll + "",
                                         find : { _id : 0 },
                                         to : shards[1]._id }) );

var failVersion = st.shard0.getDB("admin").runCommand({ getShardVersion : coll.toString() });

assert.eq(version.global, failVersion.global);

jsTest.log( "DONE!" );

st.stop();
