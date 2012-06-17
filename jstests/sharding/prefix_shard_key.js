// Test that you can shard and move chunks around with a shard key that's
// only a prefix of an existing index

var s = new ShardingTest({ name : jsTestName(), shards : 2 });

s.stopBalancer();

var db = s.getDB( "test" );
var admin = s.getDB( "admin" );
var coll = db.foo;

s.adminCommand( { enablesharding : "test" } );

for( i=0 ; i<100; i++){
    coll.save( {num : i} );
    coll.save( {num : i+100 , x : i})
}
db.getLastError();

//no usable index yet, should throw
assert.throws( function(){ s.adminCommand( { shardCollection : coll.getFullName(), key : { num : 1 } } ) } )

//create usable index
coll.ensureIndex({num : 1, x : 1});
db.getLastError();

//usable index, but doc with empty 'num' value, so still should throw
coll.save({x : -5});
assert( ! db.getLastError() , "save bad value didn't succeed");
assert.throws( function(){ s.adminCommand( { shardCollection : coll.getFullName(), key : { num : 1 } } ) } )

//remove the bad doc.  now should finally succeed
coll.remove( {x : -5});
assert( ! db.getLastError() , "remove bad value didn't succeed");
var result1 = admin.runCommand( { shardCollection : coll.getFullName(), key : { num : 1 } } );
printjson( result1 );
assert.eq( 1, result1.ok , "sharding didn't succeed");

//make sure extra index is not created
assert.eq( 2, coll.getIndexes().length );

//test splitting
var result2 = admin.runCommand( { split : coll.getFullName() , middle : { num : 50 } } );
printjson( result2 );
assert.eq( 1, result2.ok , "splitting didn't succeed");

//test moving
var result3 = admin.runCommand( { movechunk : coll.getFullName() , find : { num : 20 } , to : s.getOther( s.getServer( "test" ) ).name } );
printjson( result3 );
assert.eq( 1, result3.ok , "moveChunk didn't succeed");

s.stop();
