/*
baseName = "jstests_disk_norepeat";

ports = allocatePorts( 1 );
m = startMongod( "--port", ports[ 0 ], "--deDupMem", "200", "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" );

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
// apparently this means we also request 2 in subsequent getMore's
c = t.find().hint( {i:1} ).limit( 2 );
assert.eq( 0, c.next().i );
t.update( { i: 0 }, { i: 10 } );
for( i = 1; i < 10; ++i ) {
    if ( i == 7 ) {
        t.update( { i: 6 }, { i: 11 } );
        t.update( { i: 9 }, { i: 12 } );
    }
    if ( i == 9 ) {
        i = 12;
    }
    assert.eq( i, c.next().i );
}
assert.throws( function() { c.next() }, [], "unexpected: object found" );

m.getDB( "local" ).getCollectionNames().forEach( function( x ) { assert( !x.match( /^temp/ ), "temp collection found" ); } );

t.drop();
m.getDB( baseName ).createCollection( baseName, { capped:true, size:100000, autoIndexId:false } );
t = m.getDB( baseName ).getCollection( baseName );
t.insert( {_id:"a"} );
t.insert( {_id:"a"} );
t.insert( {_id:"a"} );

c = t.find().limit( 2 );
assert.eq( "a", c.next()._id );
assert.eq( "a", c.next()._id );
assert.eq( "a", c.next()._id );
assert( !c.hasNext() );

assert( t.validate().valid );
*/
