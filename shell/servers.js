

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

createMongoArgs = function( binaryName , args ){
    var fullArgs = [ binaryName ];

    if ( args.length == 1 && isObject( args[0] ) ){
        var o = args[0];
        for ( var k in o ){
            fullArgs.push( "--" + k );
            fullArgs.push( "" + o[k] );
        }
    }
    else {
        for ( var i=0; i<args.length; i++ )
            fullArgs.push( args[i] )
    }

    return fullArgs;
}

// Start a mongod instance and return a 'Mongo' object connected to it.
// This function's arguments are passed as command line arguments to mongod.
// The specified 'dbpath' is cleared if it exists, created if not.
startMongod = function(){

    var args = createMongoArgs( "mongod" , arguments );
    
    var dbpath = _parsePath.apply( null, args );
    resetDbpath( dbpath );

    return startMongoProgram.apply( null, args );
}

startMongos = function(){
    return startMongoProgram.apply( null, createMongoArgs( "mongos" , arguments ) );
}

// Start a mongo program instance (generally mongod or mongos) and return a
// 'Mongo' object connected to it.  This function's first argument is the
// program name, and subsequent arguments to this function are passed as
// command line arguments to the program.
startMongoProgram = function(){
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

