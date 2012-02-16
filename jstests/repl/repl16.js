// Test deduping of new documents with, and without, _ids.
// SERVER-4940

if ( 0 ) {

function doTest( insert ) {
        
    rt = new ReplTest( "repl16tests" );
    master = rt.start( true );
    master.getDB( 'd' ).createCollection( 'c', { capped:true, size:5*1024 } );
    mc = master.getDB( 'd' )[ 'c' ];

    insert( {a:1} );
    insert( {a:2} );

    slave = rt.start( false );
    sc = slave.getDB( 'd' )[ 'c' ];

    // Wait for the slave to copy the documents.
    assert.soon( function() { return sc.count() == 2; } );
    
    insert( {a:1} );
    insert( {a:2} );
    insert( {a:3} );
    assert.eq( 5, mc.count() );
    
    // Wait for the slave to apply the operations.
    assert.soon( function() { return sc.count() == 5; } );

    rt.stop();

}

function insertWithIds( obj ) {
    mc.insert( obj );
}

function insertWithoutIds( obj ) {
    mc._mongo.insert( mc._fullName, obj );
}

doTest( insertWithIds );
doTest( insertWithoutIds );

}
