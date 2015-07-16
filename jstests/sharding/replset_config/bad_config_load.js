//
// Test for what happens when config servers are down and the database config is loaded
// Should fail sanely
// Note: Test uses only 2.0 compatible features to make backport easier.
//

var st = new ShardingTest({ shards : 2, mongos : 1, sync : false })

var mongos = st.s
var coll = mongos.getCollection( "foo.bar" )

mongos.getDB( "admin" ).runCommand({ setParameter : 1, logLevel : 2 })
mongos.getDB( "admin" ).runCommand({ movePrimary : coll.getDB() + "", to : "shard0001" })

// Need to start two shards and remove one (which is also the config server) b/c 2.0 branch
// ShardingTest annoyingly doesn't have non-replica set separateConfig options
mongos.getDB( "admin" ).runCommand({ removeShard : "shard0000" })
mongos.getDB( "admin" ).runCommand({ removeShard : "shard0000" })

// Make sure mongos has no database info currently loaded
mongos.getDB( "admin" ).runCommand({ flushRouterConfig : 1 })

jsTestLog( "Setup complete!" )
st.printShardingStatus()

jsTestLog( "Stopping config servers" );
st.configRS.stopSet();

jsTestLog( "Config flushed and config servers down!" )

// Throws transport error first and subsequent times when loading config data, not no primary
for( var i = 0; i < 2; i++ ){
    try {
        coll.findOne()
        // Should always throw
        assert( false )
    }
    catch( e ){
        
        printjson( e )
        
        // Make sure we get a transport error, and not a no-primary error
        // Unfortunately e gets stringified so we have to test this way
        assert(e.message.indexOf("10276") >= 0 ||       // Transport error
               e.message.indexOf("13328") >= 0 ||       // Connect error
               e.message.indexOf("13639") >= 0 ||       // Connect error to replSet primary
               e.message.indexOf("network error") >= 0 ||
               e.message.indexOf("socket") >= 0 )
    }
}

jsTestLog( "Done!" )

st.stop()
