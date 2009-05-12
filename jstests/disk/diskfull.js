port = allocatePorts( 1 )[ 0 ];
doIt = true;
try {
    m = startMongoProgram( "mongod", "--port", port, "--dbpath", "/data/db/diskfulltest", "--nohttpinterface" );
} catch ( e ) {
    print( "path /data/db/diskfulltest/ missing, skipping diskfull test" );
    doIt = false;
}

if ( doIt ) {
    m.getDB( "diskfulltest" ).getCollection( "diskfulltest" ).save( { a: 6 } );
    assert.soon( function() { return rawMongoProgramOutput().match( /dbexit: really exiting now/ ); } );
}
