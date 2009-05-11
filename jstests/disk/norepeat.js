baseName = "jstests_disk_norepeat";

ports = allocatePorts( 1 );
m = startMongod( "--port", ports[ 0 ], "--deDupMem", "200", "--dbpath", "/data/db/" + baseName );

t = m.getDB( baseName ).getCollection( baseName );

t.drop();
t.ensureIndex( { i: 1 } );
for( i = 0; i < 3; ++i ) {
    t.save( { i: i } );
}

c = t.find().hint( { i: 1 } ).limit( 2 );
assert.eq( 0, c.next().i );
t.update( { i: 0 }, { i: 3 } );
assert.eq( 1, c.next().i );
assert.eq( 2, c.next().i );
assert.throws( function() { c.next() }, [], "unexpected: object found" );

// now force upgrade to disk storage

t.drop();
t.ensureIndex( { i: 1 } );
for( i = 0; i < 10; ++i ) {
    t.save( { i: i } );
}
c = t.find().hint( {i:1} ).limit( 2 );
assert( 10 != c.next().i );
t.update( { i: 0 }, { i: 10 } );
for( i = 1; i < 10; ++i ) {
    assert( 10 != c.next().i );
}
assert.throws( function() { c.next() }, [], "unexpected: object found" );

m.getDB( "local" ).getCollectionNames().forEach( function( x ) { assert( !x.match( /^temp/ ), "temp collection found" ); } );

assert( t.validate().valid );
