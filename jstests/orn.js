// Test dropping during an $or distinct yield SERVER-3555

t = db.jstests_orn;
t.drop();

clauses = [];
for( i = 0; i < 10; ++i ) {
    clauses.push( {a:{$lte:(i+1)*5000/10},i:49999} );
    clauses.push( {b:{$lte:(i+1)*5000/10},i:49999} );
}

p = startParallelShell( 'for( i = 0; i < 15; ++i ) { sleep( 1000 ); db.jstests_orn.drop() }' );
for( j = 0; j < 5; ++j ) {
    for( i = 0; i < 5000; ++i ) {
        t.save( {a:i,i:i} );
        t.save( {b:i,i:i} );
    }
    t.ensureIndex( {a:1} );
    t.ensureIndex( {b:1} );
    t.distinct('a',{$or:clauses});
}
p();
