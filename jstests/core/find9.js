// Test that the MaxBytesToReturnToClientAtOnce limit is enforced.

t = db.jstests_find9;
t.drop();

big = new Array( 500000 ).toString();
for( i = 0; i < 20; ++i ) {
    t.save( { a:i, b:big } );
}

// Check size limit with a simple query.
assert.eq( 20, t.find( {}, { a:1 } ).objsLeftInBatch() ); // Projection resizes the result set.
assert.gt( 20, t.find().objsLeftInBatch() );

// Check size limit on a query with an explicit batch size.
assert.eq( 20, t.find( {}, { a:1 } ).batchSize( 30 ).objsLeftInBatch() );
assert.gt( 20, t.find().batchSize( 30 ).objsLeftInBatch() );

for( i = 0; i < 20; ++i ) {
    t.save( { a:i, b:big } );
}

// Check size limit with get more.
c = t.find().batchSize( 30 );
while( c.hasNext() ) {
    assert.gt( 20, c.objsLeftInBatch() );
    c.next();
}
