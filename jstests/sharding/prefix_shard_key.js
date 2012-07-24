// Test that you can shard and move chunks around with a shard key that's
// only a prefix of an existing index

var s = new ShardingTest({ name : jsTestName(), shards : 2 });

var db = s.getDB( "test" );
var admin = s.getDB( "admin" );
var coll = db.foo;

s.adminCommand( { enablesharding : "test" } );

var longStr = 'a';
while ( longStr.length < 1024 * 128 ) { longStr += longStr; }
for( i=0 ; i<100; i++){
    coll.save( {num : i, str : longStr} );
    coll.save( {num : i+100 , x : i, str : longStr})
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

// make sure balancing happens
s.awaitBalance( coll.getName(), db.getName() );
s.stopBalancer();

//test splitting
var result2 = admin.runCommand( { split : coll.getFullName() , middle : { num : 50 } } );
printjson( result2 );
assert.eq( 1, result2.ok , "splitting didn't succeed");

//test moving
var result3 = admin.runCommand( { movechunk : coll.getFullName() , find : { num : 20 } , to : s.getOther( s.getServer( "test" ) ).name } );
printjson( result3 );
assert.eq( 1, result3.ok , "moveChunk didn't succeed");


// Test that inserting array values fails because we don't support multi-key indexes for the shard key
coll.save({ num : [1,2], x : 1});
err = db.getLastError();
print( err );
assert.neq( null, err, "Inserting document with array value for shard key succeeded");

// Because of SERVER-6095, making the index a multi-key index (on a value that *isn't* part of the
// shard key) makes that index unusable for migrations.  Test that removing the multi-key value and
// rebuilding the index allows it to be used again
coll.save({ num : 100, x : [1,2]});
var result4 = admin.runCommand( { movechunk : coll.getFullName() , find : { num : 70 } , to : s.getOther( s.getServer( "test" ) ).name } );
printjson( result4 );
assert.eq( 0, result4.ok , "moveChunk succeeded without a usable index");

coll.remove({ num : 100 });
db.getLastError();
coll.reIndex();
db.getLastError();
var result4 = admin.runCommand( { movechunk : coll.getFullName() , find : { num : 70 } , to : s.getOther( s.getServer( "test" ) ).name } );
printjson( result4 );
assert.eq( 1, result4.ok , "moveChunk failed after rebuilding index");


s.stop();
