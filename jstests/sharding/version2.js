// version2.js

s = new ShardingTest( "version2" , 1 , 2 )

s.adminCommand( { enablesharding : "alleyinsider" } );
s.adminCommand( { shardcollection : "alleyinsider.foo" , key : { num : 1 } } );
s.adminCommand( { shardcollection : "alleyinsider.bar" , key : { num : 1 } } );

a = s._connections[0].getDB( "admin" );

// setup from one client

assert.eq( a.runCommand( { "getShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).mine.i, 0 );
assert.eq( a.runCommand( { "getShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).global.i, 0 );

assert( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , authoritative : true ,
                        version : new NumberLong( 4294967296 ), // 1|0
                        shard: "shard0000" , shardHost: "localhost:30000" } ).ok == 1 );

printjson( s.config.chunks.findOne() );

assert.eq( a.runCommand( { "getShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).mine.t, 1 );
assert.eq( a.runCommand( { "getShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).global.t, 1 );

// from another client

a2 = connect( s._connections[0].name + "/admin" );

assert.eq( a2.runCommand( { "getShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).global.t , 1 , "a2 global 1" );
assert.eq( a2.runCommand( { "getShardVersion" : "alleyinsider.foo" , configdb : s._configDB } ).mine.i , 0 , "a2 mine 1" );

function simpleFindOne(){
    return a2.getMongo().getDB( "alleyinsider" ).foo.findOne();
}

assert.commandWorked( a2.runCommand( { "setShardVersion" : "alleyinsider.bar" , configdb : s._configDB , version : new NumberLong( 4294967296 ) , authoritative : true } ) , "setShardVersion bar temp");

assert.throws( simpleFindOne , [] , "should complain about not in sharded mode 1" );


// the only way that setSharVersion passes is if the shard agrees with the version
// the shard takes its version from config directly
// TODO bump timestamps in config
// assert( a2.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 2 } ).ok == 1 , "setShardVersion a2-1");

// simpleFindOne(); // now should run ok

// assert( a2.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB , version : 3 } ).ok == 1 , "setShardVersion a2-2");

// simpleFindOne(); // newer version is ok


s.stop();
