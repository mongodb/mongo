// Unsorted plan on {a:1}, sorted plan on {b:1}.  The unsorted plan exhausts its memory limit before
// the sorted plan is chosen by the query optimizer.

t = db.jstests_sortf;
t.drop();

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

for( i = 0; i < 100; ++i ) {
    t.save( {a:0,b:0} );
}

big = new Array( 10 * 1000 * 1000 ).toString();
for( i = 0; i < 5; ++i ) {
    t.save( {a:1,b:1,big:big} );
}

assert.eq( 5, t.find( {a:1} ).sort( {b:1} ).itcount() );
t.drop();