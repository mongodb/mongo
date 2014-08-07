// Check explain results when an in order plan is selected among mixed in order and out of order
// plans.

t = db.jstests_explaina;
t.drop();

t.ensureIndex( { a:1 } );
t.ensureIndex( { b:1 } );

for( i = 0; i < 1000; ++i ) {
    t.save( { a:i, b:i%3 } );
}

// Query with an initial set of documents.
explain1 = t.find( { a:{ $gte:0 }, b:2 } ).sort( { a:1 } ).hint( { a:1 } ).explain();
printjson(explain1);
assert.eq( 333, explain1.n, 'wrong n for explain1' );
assert.eq( 1000, explain1.nscanned, 'wrong nscanned for explain1' );

for( i = 1000; i < 2000; ++i ) {
    t.save( { a:i, b:i%3 } );
}

// Query with some additional documents.
explain2 = t.find( { a:{ $gte:0 }, b:2 } ).sort( { a:1 } ).hint ( { a:1 } ).explain();
printjson(explain2);
assert.eq( 666, explain2.n, 'wrong n for explain2' );
assert.eq( 2000, explain2.nscanned, 'wrong nscanned for explain2' );
