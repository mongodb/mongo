// Test initial sync cloning of a document that moves.
// SERVER-4942

if ( 0 ) { // SERVER-4942

rt = new ReplTest( "repl18tests" );

master = rt.start( true );
mc = master.getDB( 'd' )[ 'c' ];

big = new Array( 200 ).toString();

// Save some big docs at the beginning of the data file.
for( i = 0; i < 100; ++i ) {
    mc.save( { _id:i, b:big } );
}

for( i = 100; i < 10000; ++i ) {
    mc.save( { _id:i } );
}

moverId = 1000*1000;
mc.save( { _id:moverId, preserved:true } );

// Free up space at the beginning of the data file.
mc.remove( { _id:{ $lt:100 } } );

slave = rt.start( false );
sc = slave.getDB( 'd' )[ 'c' ];

// Wait for slave to start cloning.
assert.soon( function() { return sc.count() > 0; } );

// Add to the mover doc, causing it to be moved to the beginning of the data file.
mc.update( { _id:moverId }, { $set:{ c:big } } );

// Wait for the mover with the 'preserved' field.
assert.soon( function() { return sc.count( { _id:moverId, preserved:true } ) > 0; } );

}
