//
// Tests autosplit locations with force : true
//

var options = { separateConfig : true, 
                chunksize : 1, // MB
                mongosOptions : { noAutoSplit : "" }
              };

var st = new ShardingTest({ shards : 1, mongos : 1, other : options });
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB( "admin" );
var config = mongos.getDB( "config" );
var shardAdmin = st.shard0.getDB( "admin" );
var coll = mongos.getCollection( "foo.bar" );

assert( admin.runCommand({ enableSharding : coll.getDB() + "" }).ok );
assert( admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } }).ok );
assert( admin.runCommand({ split : coll + "", middle : { _id : 0 } }).ok );

jsTest.log( "Insert a bunch of data into a chunk of the collection..." );

for ( var i = 0; i < (250 * 1000) + 10; i++ ) {
    coll.insert({ _id : i });
}
assert.eq( null, coll.getDB().getLastError() );

jsTest.log( "Insert a bunch of data into the rest of the collection..." );

for ( var i = 1; i <= (250 * 1000); i++ ) {
    coll.insert({ _id : -i });
}
assert.eq( null, coll.getDB().getLastError() );

jsTest.log( "Get split points of the chunk using force : true..." );

var maxChunkSizeBytes = 1024 * 1024;

var splitKeys = shardAdmin.runCommand({ splitVector : coll + "",
                                        keyPattern : { _id : 1 },
                                        min : { _id : 0 },
                                        max : { _id : MaxKey },
                                        force : true
                                      }).splitKeys;

printjson( splitKeys );
printjson( coll.stats() );
st.printShardingStatus();

jsTest.log( "Make sure our split is approximately in half..." );

assert.eq( splitKeys.length, 1 );
var splitKey = splitKeys[0]._id;

assert.gt( splitKey, ((250 * 1000) / 2) - 50 );
assert.lt( splitKey, ((250 * 1000) / 2) + 50 );

st.stop();
