// test replace peer

var baseName = "jstests_replacepeertest";

ismaster = function( n ) {
    im = n.getDB( "admin" ).runCommand( { "ismaster" : 1 } );
    //    print( "ismaster: " + tojson( im ) );
    assert( im );
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
    s.setSlaveOk();
    assert.soon( function() {
                if ( -1 == s.getDBNames().indexOf( baseName ) )
                    return false;
                if ( -1 == s.getDB( baseName ).getCollectionNames().indexOf( "z" ) )
                    return false;
                return 1 == getCount( s );
                } );
}

doTest = function( signal ) {
    
    // spec small oplog for fast startup on 64bit machines
    a = startMongod( "--port", "27018", "--dbpath", "/data/db/" + baseName + "-arbiter" );
    l = startMongod( "--port", "27019", "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:27021", "127.0.0.1:27018", "--oplogSize", "1" );
    r = startMongod( "--port", "27021", "--dbpath", "/data/db/" + baseName + "-right", "--pairwith", "127.0.0.1:27019", "127.0.0.1:27018", "--oplogSize", "1" );
    
    assert.soon( function() {
                am = ismaster( a );
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( am == 1 );
                assert( lm == -1 || lm == 0 );
                assert( rm == -1 || rm == 0 || rm == 1 );
                
                return ( lm == 0 && rm == 1 );
                } );
    
    checkWrite( r, l );
    
    stopMongod( 27019, signal );

    writeOne( r );
    
    assert.eq( 1, r.getDB( "admin" ).runCommand( {replacepeer:1} ).ok );
    
    writeOne( r );
    sleep( 1000 ); // flush write to disk
    
    stopMongod( 27021, signal );
    
    l = startMongod( "--port", "27020", "--dbpath", "/data/db/" + baseName + "-left", "--pairwith", "127.0.0.1:27021", "127.0.0.1:27018", "--oplogSize", "1" );
    r = startMongoProgram( "mongod", "--port", "27021", "--dbpath", "/data/db/" + baseName + "-right", "--pairwith", "127.0.0.1:27020", "127.0.0.1:27018", "--oplogSize", "1" );

    assert.soon( function() {
                am = ismaster( a );
                lm = ismaster( l );
                rm = ismaster( r );
                
                assert( am == 1 );
                assert( lm == -1 || lm == 0 );
                assert( rm == -1 || rm == 0 || rm == 1 );
                
                return ( lm == 0 && rm == 1 );
                } );
    
    checkWrite( r, l );
    l.setSlaveOk();
    assert.eq( 4, l.getDB( baseName ).z.find().toArray().length );
    
    stopMongod( 27018 );
    stopMongod( 27020 );
    stopMongod( 27021 );    
}

doTest( 15 ); // SIGTERM
doTest( 9 );  // SIGKILL
