// Test persistence of list of dbs to add.

doTest = function( signal ) {

    rt = new ReplTest( "repl7tests" );
    
    m = rt.start( true );

    for( n = "a"; n != "aaaaa"; n += "a" ) {
        m.getDB( n ).a.save( {x:1} );
    }

    s = rt.start( false );    
    
    assert.soon( function() {
                return -1 != s.getDBNames().indexOf( "aa" );
                }, "aa timeout", 60000, 1000 );
    
    rt.stop( false, signal );

    s = rt.start( false, null, signal );
    
    assert.soon( function() {
                for( n = "a"; n != "aaaaa"; n += "a" ) {
                    if ( -1 == s.getDBNames().indexOf( n ) )
                        return false;                    
                }
                return true;
                }, "a-aaaa timeout", 60000, 1000 );

    assert.soon( function() {
                for( n = "a"; n != "aaaaa"; n += "a" ) {
                    if ( 1 != m.getDB( n ).a.find().count() ) {
                        return false;
                    }
                }
                return true; }, "a-aaaa count timeout" );

    sleep( 300 );
    
    rt.stop();
}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
