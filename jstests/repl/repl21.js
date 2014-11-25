// Test initial sync cloning of an $addToSet operation.
// SERVER-4945

if ( 0 ) { // SERVER-4945
    
rt = new ReplTest( "repl21tests" );

master = rt.start( true );
mc = master.getDB( 'd' )[ 'c' ];

for( i = 0; i < 100000; ++i ) {
    mc.insert( { _id:i, z:i } );
}

targetId = 1000*1000;
mc.insert( { _id:targetId, val:[ 1 ] } );
master.getDB( 'd' ).getLastError();

slave = rt.start( false );
sc = slave.getDB( 'd' )[ 'c' ];

// Wait for slave to start cloning.
assert.soon( function() { c = sc.count(); /*print( c );*/ return c > 0; } );

// $addToSet the number '2'.
mc.update( { _id:targetId }, { $addToSet:{ val:2 } } );
// Modify the value that was just added.
mc.update( { _id:targetId }, { $set:{ 'val.1':3 } } );

mc.insert( { _id:'sentinel' } );

// Wait for the updates to be applied.
assert.soon( function() { return sc.count( { _id:'sentinel' } ) > 0; } );

// Check that the val array is as expected.
assert.eq( [ 1, 3 ], mc.findOne( { _id:targetId } ).val );
assert.eq( [ 1, 3 ], sc.findOne( { _id:targetId } ).val );

}