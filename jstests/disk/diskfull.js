doIt = false;
files = listFiles( "/data/db" );
for ( i in files ) {
    if ( files[ i ].name == "/data/db/diskfulltest" ) {
        doIt = true;
    }
}

if ( !doIt ) {
    print( "path /data/db/diskfulltest/ missing, skipping diskfull test" );
    doIt = false;
}

if ( doIt ) {
    port = allocatePorts( 1 )[ 0 ];
    m = startMongoProgram( "mongod", "--port", port, "--dbpath", "/data/db/diskfulltest", "--nohttpinterface", "--bind_ip", "127.0.0.1" );
    m.getDB( "diskfulltest" ).getCollection( "diskfulltest" ).save( { a: 6 } );
    assert.soon( function() { return rawMongoProgramOutput().match( /dbexit: really exiting now/ ); }, "didn't see 'really exiting now'" );
    assert( !rawMongoProgramOutput().match( /Got signal/ ), "saw 'Got signal', not expected.  Output: " + rawMongoProgramOutput() );
}
