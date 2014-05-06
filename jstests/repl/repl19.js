// Test initial sync cloning of a $pop operation.
// SERVER-4943

if ( 0 ) { // SERVER-4943

rt = new ReplTest( "repl19tests" );

master = rt.start( true );
mc = master.getDB( 'd' )[ 'c' ];

for( i = 0; i < 100000; ++i ) {
    mc.insert( { _id:i, z:i } );
}

targetId = 1000*1000;
mc.insert( { _id:targetId, val:[ 1, 2, 3 ] } );
master.getDB( 'd' ).getLastError();

slave = rt.start( false );
sc = slave.getDB( 'd' )[ 'c' ];

// Wait for slave to start cloning.
assert.soon( function() { c = sc.count(); /*print( c );*/ return c > 0; } );

// $pop first element of val.
mc.update( { _id:targetId }, { $pop:{ val:-1 } } );
// $push another element to val.
mc.update( { _id:targetId }, { $push:{ val:4 } } );

mc.insert( { _id:'sentinel' } );

// Wait for the updates to be applied.
assert.soon( function() { return sc.count( { _id:'sentinel' } ) > 0; } );

// Check that the val array is as expected.
assert.eq( [ 2, 3, 4 ], mc.findOne( { _id:targetId } ).val );
assert.eq( [ 2, 3, 4 ], sc.findOne( { _id:targetId } ).val );

}
