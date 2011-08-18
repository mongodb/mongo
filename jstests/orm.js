// Test dropping during a $or yield SERVER-3555

t = db.jstests_orm;
t.drop();

clauses = [];
for( i = 0; i < 10; ++i ) {
    clauses.push( {a:{$lte:(i+1)*5000/10},i:49999} );
    clauses.push( {b:{$lte:(i+1)*5000/10},i:49999} );
}

p = startParallelShell( 'for( i = 0; i < 15; ++i ) { sleep( 1000 ); db.jstests_orm.drop() }' );
for( j = 0; j < 5; ++j ) {
    for( i = 0; i < 5000; ++i ) {
	t.save( {a:i,i:i} );
	t.save( {b:i,i:i} );
    }
    t.ensureIndex( {a:1} );
    t.ensureIndex( {b:1} );
    try {
      t.find( {$or:clauses} ).itcount();
      t.find( {$or:clauses} ).count();
      t.update( {$or:clauses}, {} );
      t.remove( {$or:clauses} );
    } catch ( e ) {
    }
    db.getLastError();
}
p();
