

_parsePath = function() {
    var dbpath = "";
    for( var i = 0; i < arguments.length; ++i )
        if ( arguments[ i ] == "--dbpath" )
            dbpath = arguments[ i + 1 ];
    
    if ( dbpath == "" )
        throw "No dbpath specified";
    
    return dbpath;
}

_parsePort = function() {
    var port = "";
    for( var i = 0; i < arguments.length; ++i )
        if ( arguments[ i ] == "--port" )
            port = arguments[ i + 1 ];
    
    if ( port == "" )
        throw "No port specified";
    return port;
}

// Start a mongod instance and return a 'Mongo' object connected to it.
// This function's arguments are passed as command line arguments to mongod.
// The specified 'dbpath' is cleared if it exists, created if not.
startMongod = function() {
    var dbpath = _parsePath.apply( null, arguments );
    resetDbpath( dbpath );
    var fullArgs = Array();
    fullArgs[ 0 ] = "mongod";
    for( i = 0; i < arguments.length; ++i )
        fullArgs[ i + 1 ] = arguments[ i ];
    return startMongoProgram.apply( null, fullArgs );
}

// Start a mongo program instance (generally mongod or mongos) and return a
// 'Mongo' object connected to it.  This function's first argument is the
// program name, and subsequent arguments to this function are passed as
// command line arguments to the program.
startMongoProgram = function() {
    var port = _parsePort.apply( null, arguments );

    _startMongoProgram.apply( null, arguments );
    
    var m;
    assert.soon
    ( function() {
        try {
            m = new Mongo( "127.0.0.1:" + port );
            return true;
        } catch( e ) {
        }
        return false;
    } );
    
    return m;
}

