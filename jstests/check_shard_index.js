// -------------------------
//  CHECKSHARDINGINDEX TEST UTILS
// -------------------------

f = db.jstests_shardingindex;
f.drop();


// -------------------------
// Case 1: all entries filled or empty should make a valid index
//

f.drop();
f.ensureIndex( { x: 1 , y: 1 } );
assert.eq( 0 , f.count() , "1. initial count should be zero" );

res = db.runCommand( { checkShardingIndex: "test.jstests_shardingindex" , keyPattern: {x:1, y:1} , force: true });
assert.eq( true , res.ok, "1a" );

f.save( { x: 1 , y : 1 } );
assert.eq( 1 , f.count() , "1. count after initial insert should be 1" );
res = db.runCommand( { checkShardingIndex: "test.jstests_shardingindex" , keyPattern: {x:1, y:1} , force: true });
assert.eq( true , res.ok , "1b" );


// -------------------------
// Case 2: entry with null values would make an index unsuitable
//

f.drop();
f.ensureIndex( { x: 1 , y: 1 } );
assert.eq( 0 , f.count() , "2. initial count should be zero" );

f.save( { x: 1 , y : 1 } );
f.save( { y: 2 } );
assert.eq( 2 , f.count() , "2. count after initial insert should be 2" );
res = db.runCommand( { checkShardingIndex: "test.jstests_shardingindex" , keyPattern: {x:1, y:1} , force: true });
assert.eq( false , res.ok , "2a" );

print("PASSED");
