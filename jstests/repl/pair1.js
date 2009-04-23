// Basic pairing test

var baseName = "jstests_pair1test";

ismaster = function( n ) {
    var im = n.getDB( "admin" ).runCommand( { "ismaster" : 1 } );
//    print( "ismaster: " + tojson( im ) );
    assert( im, "command ismaster failed" );
    return im.ismaster;
}

var writeOneIdx = 0;

writeOne = function( n ) {
    n.getDB( baseName ).z.save( { _id: new ObjectId(), i: ++writeOneIdx } );
}

getCount = function( n ) {
    return n.getDB( baseName ).z.find( { i: writeOneIdx } ).toArray().length;
}

checkWrite = function( m, s ) {
    writeOne( m );
    assert.eq( 1, getCount( m ) );
    check( s );
}

check = function( s ) {
    s.setSlaveOk();
    assert.soon( function() {
                if ( -1 == s.getDBNames().indexOf( baseName ) )
                    return false;
                if ( -1 == s.getDB( baseName ).getCollectionNames().indexOf( "z" ) )
                    return false;
                return 1 == getCount( s );
                } );    
}

// check that slave reads and writes are guarded
checkSlaveGuard = function( s ) {
    var t = s.getDB( baseName + "-temp" ).temp;
    assert.throws( t.find().count, {}, "not master" );
    assert.throws( t.find(), {}, "not master", "find did not assert" );
    
    checkError = function() {
        assert.eq( "not master", s.getDB( "admin" ).getLastError() );
        s.getDB( "admin" ).resetError();
    }
    s.getDB( "admin" ).resetError();
    t.save( {x:1} );
    checkError();
    t.update( {}, {x:2}, true );
    checkError();
    t.remove( {x:0} );
    checkError();
}

doTest = function( signal ) {

    ports = allocatePorts( 3 );
    
    // spec small oplog for fast startup on 64bit machines
    a = startMongod( "--port", ports[ 0 ], "--dbpath", "/data/db/" + baseName + "-arbiter", "--nohttpinterface" );
    l = startMongod( "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:" + ports[ 2 ], "127.0.0.1:" + ports[ 0 ], "--oplogSize", "1", "--nohttpinterface" );
    r = startMongod( "--port", ports[ 2 ], "--dbpath", "/data/db/" + baseName + "-right", "--pairwith", "127.0.0.1:" + ports[ 1 ], "127.0.0.1:" + ports[ 0 ], "--oplogSize", "1", "--nohttpinterface" );
    
    assert.soon( function() {
                am = ismaster( a );
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( am == 1, "am value invalid" );
                assert( lm == -1 || lm == 0, "lm value invalid" );
                assert( rm == -1 || rm == 0 || rm == 1, "rm value invalid" );
                
                return ( lm == 0 && rm == 1 );
                } );
    
    checkSlaveGuard( l );
    
    checkWrite( r, l );
    
    stopMongod( ports[ 2 ], signal );
    
    assert.soon( function() {
                lm = ismaster( l );
                assert( lm == 0 || lm == 1, "lm value invalid" );
                return ( lm == 1 );
                } );
    
    writeOne( l );
    
    r = startMongoProgram( "mongod", "--port", ports[ 2 ], "--dbpath", "/data/db/" + baseName + "-right", "--pairwith", "127.0.0.1:" + ports[ 1 ], "127.0.0.1:" + ports[ 0 ], "--oplogSize", "1", "--nohttpinterface" );
    
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == 1, "lm value invalid" );
                assert( rm == -1 || rm == 0, "rm value invalid" );
                
                return ( rm == 0 );
                } );
    
    // Once this returns, the initial sync for r will have completed.
    check( r );
    
    checkWrite( l, r );
    
    stopMongod( ports[ 1 ], signal );
    
    assert.soon( function() {
                rm = ismaster( r );
                assert( rm == 0 || rm == 1, "rm value invalid" );
                return ( rm == 1 );
                } );
    
    l = startMongoProgram( "mongod", "--port", ports[ 1 ], "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:" + ports[ 2 ], "127.0.0.1:" + ports[ 0 ], "--oplogSize", "1", "--nohttpinterface" );
    
    assert.soon( function() {
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( lm == -1 || lm == 0, "lm value invalid" );
                assert( rm == 1, "rm value invalid" );
                
                return ( lm == 0 && rm == 1 );
                } );
    
    checkWrite( r, l );

    ports.forEach( function( x ) { stopMongod( x ); } );

}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
