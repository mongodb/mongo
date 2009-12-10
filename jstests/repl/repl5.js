// Test auto reclone after failed initial clone

soonCountAtLeast = function( db, coll, count ) {
    assert.soon( function() { 
//                print( "count: " + s.getDB( db )[ coll ].find().count() );
                return s.getDB( db )[ coll ].find().count() >= count; 
                } );    
}

doTest = function( signal ) {

    rt = new ReplTest( "repl5tests" );
    
    m = rt.start( true );
    
    ma = m.getDB( "a" ).a;
    for( i = 0; i < 10000; ++i )
        ma.save( { i:i } );
    
    s = rt.start( false );
    soonCountAtLeast( "a", "a", 1 );
    rt.stop( false, signal );

    s = rt.start( false, null, true );
    sleep( 1000 );
    soonCountAtLeast( "a", "a", 10000 );

    rt.stop();
}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
