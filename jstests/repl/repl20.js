// Test initial sync cloning of a $pull operation.
// SERVER-4944

if ( 0 ) { // SERVER-4944
    
rt = new ReplTest( "repl20tests" );

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

// $pull the '1' element.
mc.update( { _id:targetId }, { $pull:{ val:1 } } );
// $push a new '1' element.
mc.update( { _id:targetId }, { $push:{ val:1 } } );
// $push a new '2' element.
mc.update( { _id:targetId }, { $push:{ val:2 } } );

mc.insert( { _id:'sentinel' } );

// Wait for the updates to be applied.
assert.soon( function() { return sc.count( { _id:'sentinel' } ) > 0; } );

// Check that the val array is as expected.
assert.eq( [ 1, 2 ], mc.findOne( { _id:targetId } ).val );
assert.eq( [ 1, 2 ], sc.findOne( { _id:targetId } ).val );

}
