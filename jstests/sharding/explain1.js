// Check explain() results reported for a sharded cluster, in particular nscannedObjects.
// SERVER-4161

s = new ShardingTest( "explain1" , 2 , 2 );

// Tests can be invalidated by the balancer.
s.stopBalancer()

db = s.getDB( "test" );

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );

t = db.foo;
for( i = 0; i < 10; ++i ) {
    t.save( { a:i } );
}

// Without an index.
explain = t.find( { a:{ $gte:5 } } ).explain();
assert.eq( explain.cursor, 'BasicCursor' );
assert.eq( explain.n, 5 );
assert.eq( explain.nscanned, 10 );
assert.eq( explain.nscannedObjects, 10 );

// With an index.
t.ensureIndex( { a:1 } );
explain = t.find( { a:{ $gte:5 } } ).explain();
assert.eq( explain.cursor, 'BtreeCursor a_1' );
assert.eq( explain.n, 5 );
assert.eq( explain.nscanned, 5 );
assert.eq( explain.nscannedObjects, 5 );

// With a covered index.
t.ensureIndex( { a:1 } );
explain = t.find( { a:{ $gte:5 } }, { _id:0, a:1 } ).explain();
assert.eq( explain.cursor, 'BtreeCursor a_1' );
assert.eq( explain.n, 5 );
assert.eq( explain.nscanned, 5 );
assert.eq( explain.nscannedObjects, 5 ); // Covered indexes do not work with sharding.

s.stop();
