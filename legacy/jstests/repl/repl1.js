// Test basic replication functionality

var baseName = "jstests_repl1test";

soonCount = function( count ) {
    assert.soon( function() { 
//                print( "check count" );
//                print( "count: " + s.getDB( baseName ).z.find().count() );
                return s.getDB( baseName ).a.find().count() == count; 
                } );    
}

doTest = function( signal ) {

    rt = new ReplTest( "repl1tests" );
    
    m = rt.start( true );
    s = rt.start( false );
    
    am = m.getDB( baseName ).a
    
    for( i = 0; i < 1000; ++i )
        am.save( { _id: new ObjectId(), i: i } );

    soonCount( 1000 );
    as = s.getDB( baseName ).a    
    assert.eq( 1, as.find( { i: 0 } ).count() );
    assert.eq( 1, as.find( { i: 999 } ).count() );

    rt.stop( false, signal );
    
    for( i = 1000; i < 1010; ++i )
        am.save( { _id: new ObjectId(), i: i } );

    s = rt.start( false, null, true );
    soonCount( 1010 );
    as = s.getDB( baseName ).a
    assert.eq( 1, as.find( { i: 1009 } ).count() );

    rt.stop( true, signal );
    
    m = rt.start( true, null, true );
    am = m.getDB( baseName ).a

    for( i = 1010; i < 1020; ++i )
        am.save( { _id: new ObjectId(), i: i } );

    assert.soon( function() { return as.find().count() == 1020; } );
    assert.eq( 1, as.find( { i: 1019 } ).count() );

    assert.automsg( "m.getDB( 'local' ).getCollection( 'oplog.$main' ).stats().size > 0" );
    
    rt.stop();
}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
