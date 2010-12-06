// version1.js

s = new ShardingTest( "version1" , 1 , 2 )

s.adminCommand( { enablesharding : "alleyinsider" } );
s.adminCommand( { shardcollection : "alleyinsider.foo" , key : { num : 1 } } );

// alleyinsider.foo is supposed to have one chunk, version 1|0, in shard000
s.printShardingStatus();

a = s._connections[0].getDB( "admin" );

assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).ok == 0 );

assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : "a" } ).ok == 0 );

assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , authoritative : true } ).ok == 0 );

assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 2 } ).ok == 0 ,
        "should have failed b/c no auth" );

assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 2 , authoritative : true } ) ,
        "should have failed because first setShardVersion needs shard info" );

assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 2 , authoritative : true , 
                         shard: "shard0000" , shardHost: "localhost:30000" } ) , 
        "should have failed because version is config is 1|0" );

assert.commandWorked( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , 
                                      version : new NumberLong( 4294967296 ), // 1|0
                                      authoritative : true , shard: "shard0000" , shardHost: "localhost:30000" } ) , 
                     "should have worked" );

assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : "a" , version : 2 } ).ok == 0 , "A" );

assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 2 } ).ok == 0 , "B" );
assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 1 } ).ok == 0 , "C" );

// the only way that setSharVersion passes is if the shard agrees with the version
// the shard takes its version from config directly
// TODO bump timestamps in config
// assert.eq( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 3 } ).oldVersion.i , 2 , "oldVersion" );

// assert.eq( a.runCommand( { "getShardVersion" : "alleyinsider.foo" } ).mine.i , 3 , "my get version A" );
// assert.eq( a.runCommand( { "getShardVersion" : "alleyinsider.foo" } ).global.i , 3 , "my get version B" );

s.stop();
