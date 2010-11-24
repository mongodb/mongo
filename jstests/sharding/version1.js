// version1.js

s = new ShardingTest( "version1" , 1 , 2 )

s.adminCommand( { enablesharding : "alleyinsider" } );
s.adminCommand( { shardcollection : "alleyinsider.foo" , key : { num : 1 } } );

a = s._connections[0].getDB( "admin" );

assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).ok == 0 );
assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : "a" } ).ok == 0 );
assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , authoritative : true } ).ok == 0 );
assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 2 } ).ok == 0 , "should have failed b/c no auth" );

assert.commandWorked( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 2 , authoritative : true } ) , "should have worked" );
assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : "a" , version : 2 } ).ok == 0 );

assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 2 } ).ok == 1 );
assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 1 } ).ok == 0 );

assert.eq( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 3 } ).oldVersion.i , 2 , "oldVersion" );

assert.eq( a.runCommand( { "getShardVersion" : "alleyinsider.foo" } ).mine.i , 3 , "my get version A" );
assert.eq( a.runCommand( { "getShardVersion" : "alleyinsider.foo" } ).global.i , 3 , "my get version B" );

s.stop();
