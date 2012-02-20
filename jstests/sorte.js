// Check that getLastError is clear after a scan and order plan triggers an in memory sort
// exception, but an in order plan continues running.
// SERVER-5016

if ( 0 ) { // SERVER-5016

t = db.jstests_sorte;
t.drop();

big = new Array( 1000000 ).toString()

for( i = 0; i < 300; ++i ) {
    t.save( { a:0, b:0 } );
}

for( i = 0; i < 40; ++i ) {
    t.save( { a:1, b:1, big:big } );
}

t.ensureIndex( { a:1 } );
t.ensureIndex( { b:1 } );

c = t.find( { a:{ $gte:0 }, b:1 } ).sort( { a:1 } );
c.next();
assert( !db.getLastError() );
count = 1;
count += c.itcount();
assert.eq( 40, count );

}