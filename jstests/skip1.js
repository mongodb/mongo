// SERVER-2845 When skipping objects without loading them, they shouldn't be
// included in the nscannedObjects count.

if ( 0 ) { // SERVER-2845
t = db.jstests_skip1;
t.drop();

t.ensureIndex( {a:1} );
t.save( {a:5} );
t.save( {a:5} );
t.save( {a:5} );

assert.eq( 3, t.find( {a:5} ).skip( 2 ).explain().nscanned );
assert.eq( 1, t.find( {a:5} ).skip( 2 ).explain().nscannedObjects );
}