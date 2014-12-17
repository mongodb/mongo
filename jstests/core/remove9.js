// SERVER-2009 Count odd numbered entries while updating and deleting even numbered entries.

t = db.jstests_remove9;
t.drop();
t.ensureIndex( {i:1} );
for( i = 0; i < 1000; ++i ) {
	t.save( {i:i} );
}

s = startParallelShell( 't = db.jstests_remove9; for( j = 0; j < 5000; ++j ) { i = Random.randInt( 499 ) * 2; t.update( {i:i}, {$set:{i:2000}} ); t.remove( {i:2000} ); t.save( {i:i} ); }' );

for( i = 0; i < 1000; ++i ) {
	assert.eq( 500, t.find( {i:{$gte:0,$mod:[2,1]}} ).hint( {i:1} ).itcount() );
}

s();
