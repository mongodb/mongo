// Test that you can shard with shard key that's only a prefix of an existing index.
//
// Part 1: Shard new collection on {num : 1} with an index on {num : 1, x : 1}.
//         Test that you can split and move chunks around.
// Part 2: Test that adding an array value for x makes it unusuable. Deleting the
//         array value and re-indexing makes it usable again.
// Part 3: Shard new collection on {skey : 1} but with a longer index.
//         Insert docs with same val for 'skey' but different vals for 'extra'.
//         Move chunks around and check that [min,max) chunk boundaries are properly obeyed.

var s = new ShardingTest({ name : jsTestName(), shards : 2 });

var db = s.getDB( "test" );
var admin = s.getDB( "admin" );
var config = s.getDB( "config" );
var shards = config.shards.find().toArray();
var shard0 = new Mongo( shards[0].host );
var shard1 = new Mongo( shards[1].host );

s.adminCommand( { enablesharding : "test" } );

//******************Part 1********************

var coll = db.foo;

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

// Make sure our initial balance cleanup doesn't interfere with later migrations.
assert.soon( function(){
    print( "Waiting for migration cleanup to occur..." );
    return coll.count() == coll.find().itcount();
})

s.stopBalancer();

//test splitting
var result2 = admin.runCommand( { split : coll.getFullName() , middle : { num : 50 } } );
printjson( result2 );
assert.eq( 1, result2.ok , "splitting didn't succeed");

//test moving
var result3 = admin.runCommand({ movechunk: coll.getFullName(), find: { num: 20 },
    to: s.getOther(s.getServer("test")).name, _waitForDelete: true });
printjson( result3 );
assert.eq( 1, result3.ok , "moveChunk didn't succeed");


//******************Part 2********************

// Test that inserting array values fails because we don't support multi-key indexes for the shard key
coll.save({ num : [1,2], x : 1});
err = db.getLastError();
print( err );
assert.neq( null, err, "Inserting document with array value for shard key succeeded");

// Because of SERVER-6095, making the index a multi-key index (on a value that *isn't* part of the
// shard key) makes that index unusable for migrations.  Test that removing the multi-key value and
// rebuilding the index allows it to be used again
coll.save({ num : 100, x : [1,2]});
var result4 = admin.runCommand({ movechunk: coll.getFullName(), find: { num: 70 },
    to: s.getOther(s.getServer("test")).name, _waitForDelete: true });
printjson( result4 );
assert.eq( 0, result4.ok , "moveChunk succeeded without a usable index");

coll.remove({ num : 100 });
db.getLastError();
coll.reIndex();
db.getLastError();
result4 = admin.runCommand({ movechunk: coll.getFullName(), find : { num : 70 },
    to: s.getOther(s.getServer("test")).name, _waitForDelete: true });
printjson( result4 );
assert.eq( 1, result4.ok , "moveChunk failed after rebuilding index");

// Make sure the previous migrates cleanup doesn't interfere with later tests
assert.soon( function(){
    print( "Waiting for migration cleanup to occur..." );
    return coll.count() == coll.find().itcount();
})

//******************Part 3********************

// Check chunk boundaries obeyed when using prefix shard key.
// This test repeats with shard key as the prefix of different longer indices.

for( i=0; i < 3; i++ ){

    // setup new collection on shard0
    var coll2 = db.foo2;
    coll2.drop();
    var moveRes = admin.runCommand( { movePrimary : coll2.getDB() + "", to : shards[0]._id } );
    assert.eq( moveRes.ok , 1 , "primary not moved correctly" );

    // declare a longer index
    if ( i == 0 ) {
        coll2.ensureIndex( { skey : 1, extra : 1 } );
    }
    else if ( i == 1 ) {
        coll2.ensureIndex( { skey : 1, extra : -1 } );
    }
    else if ( i == 2 ) {
        coll2.ensureIndex( { skey : 1, extra : 1 , superfluous : -1 } );
    }
    db.getLastError();

    // then shard collection on prefix
    var shardRes = admin.runCommand( { shardCollection : coll2 + "", key : { skey : 1 } } );
    assert.eq( shardRes.ok , 1 , "collection not sharded" );

    // insert docs with same value for skey
    for( var i = 0; i < 5; i++ ){
        for( var j = 0; j < 5; j++ ){
            coll2.insert( { skey : 0, extra : i , superfluous : j } );
        }
    }
    assert.eq( null, coll2.getDB().getLastError() , "inserts didn't work" );

    // split on that key, and check it makes 2 chunks
    var splitRes = admin.runCommand( { split : coll2 + "", middle : { skey : 0 } } );
    assert.eq( splitRes.ok , 1 , "split didn't work" );
    assert.eq( config.chunks.find( { ns : coll2.getFullName() } ).count() , 2 );

    // movechunk should move ALL docs since they have same value for skey
    moveRes = admin.runCommand({ moveChunk: coll2 + "", find: { skey: 0 },
        to: shards[1]._id, _waitForDelete: true });
    assert.eq( moveRes.ok , 1 , "movechunk didn't work" );

    // Make sure our migration eventually goes through before testing individual shards
    assert.soon( function(){
        print( "Waiting for migration cleanup to occur..." );
        return coll2.count() == coll2.find().itcount();
    })
    
    // check no orphaned docs on the shards
    assert.eq( 0 , shard0.getCollection( coll2 + "" ).find().itcount() );
    assert.eq( 25 , shard1.getCollection( coll2 + "" ).find().itcount() );

    // and check total
    assert.eq( 25 , coll2.find().itcount() , "bad total number of docs after move" );

    db.printShardingStatus();
}

s.stop();
