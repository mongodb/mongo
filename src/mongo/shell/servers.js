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

connectionURLTheSame = function( a , b ){
    
    if ( a == b )
        return true;

    if ( ! a || ! b )
        return false;
    
    if( a.host ) return connectionURLTheSame( a.host, b )
    if( b.host ) return connectionURLTheSame( a, b.host )
    
    if( a.name ) return connectionURLTheSame( a.name, b )
    if( b.name ) return connectionURLTheSame( a, b.name )
    
    if( a.indexOf( "/" ) < 0 && b.indexOf( "/" ) < 0 ){
        a = a.split( ":" )
        b = b.split( ":" )
        
        if( a.length != b.length ) return false
        
        if( a.length == 2 && a[1] != b[1] ) return false
                
        if( a[0] == "localhost" || a[0] == "127.0.0.1" ) a[0] = getHostName()
        if( b[0] == "localhost" || b[0] == "127.0.0.1" ) b[0] = getHostName()
        
        return a[0] == b[0]
    }
    else {
        var a0 = a.split( "/" )[0]
        var b0 = b.split( "/" )[0]
        return a0 == b0
    }
}

assert( connectionURLTheSame( "foo" , "foo" ) )
assert( ! connectionURLTheSame( "foo" , "bar" ) )

assert( connectionURLTheSame( "foo/a,b" , "foo/b,a" ) )
assert( ! connectionURLTheSame( "foo/a,b" , "bar/a,b" ) )

createMongoArgs = function( binaryName , args ){
    var fullArgs = [ binaryName ];

    if ( args.length == 1 && isObject( args[0] ) ){
        var o = args[0];
        for ( var k in o ){
          if ( o.hasOwnProperty(k) ){
            if ( k == "v" && isNumber( o[k] ) ){
                var n = o[k];
                if ( n > 0 ){
                    if ( n > 10 ) n = 10;
                    var temp = "-";
                    while ( n-- > 0 ) temp += "v";
                    fullArgs.push( temp );
                }
            }
            else {
                fullArgs.push( "--" + k );
                if ( o[k] != "" )
                    fullArgs.push( "" + o[k] );
            }
          }
        }
    }
    else {
        for ( var i=0; i<args.length; i++ )
            fullArgs.push( args[i] )
    }

    return fullArgs;
}


MongoRunner = function(){}
    
MongoRunner.dataDir = "/data/db"
MongoRunner.dataPath = "/data/db/"
MongoRunner.usedPortMap = {}
MongoRunner.logicalOptions = { runId : true,
                               pathOpts : true, 
                               remember : true,
                               noRemember : true,
                               appendOptions : true,
                               restart : true,
                               noCleanData : true,
                               cleanData : true,
                               startClean : true,
                               forceLock : true,
                               useLogFiles : true,
                               useHostName : true,
                               useHostname : true,
                               noReplSet : true,
                               forgetPort : true,
                               arbiter : true,
                               noJournalPrealloc : true,
                               noJournal : true }

MongoRunner.toRealPath = function( path, pathOpts ){
    
    // Replace all $pathOptions with actual values
    pathOpts = pathOpts || {}
    path = path.replace( /\$dataPath/g, MongoRunner.dataPath )
    path = path.replace( /\$dataDir/g, MongoRunner.dataDir )
    for( key in pathOpts ){
        path = path.replace( RegExp( "\\$" + RegExp.escape(key), "g" ), pathOpts[ key ] )
    }
    
    // Relative path
    if( ! path.startsWith( "/" ) ){
        if( path != "" && ! path.endsWith( "/" ) )
            path += "/"
                
        path = MongoRunner.dataPath + path
    }
    
    return path
    
}

MongoRunner.toRealDir = function( path, pathOpts ){
    
    path = MongoRunner.toRealPath( path, pathOpts )
    
    if( path.endsWith( "/" ) )
        path = path.substring( 0, path.length - 1 )
        
    return path
}

MongoRunner.toRealFile = MongoRunner.toRealDir

MongoRunner.nextOpenPort = function(){

    var i = 0;
    while( MongoRunner.usedPortMap[ "" + ( 27000 + i ) ] ) i++;
    MongoRunner.usedPortMap[ "" + ( 27000 + i ) ] = true
    
    return 27000 + i

}

/**
 * Converts the args object by pairing all keys with their value and appending
 * dash-dash (--) to the keys. The only exception to this rule are keys that
 * are defined in MongoRunner.logicalOptions, of which they will be ignored.
 * 
 * @param {string} binaryName
 * @param {Object} args
 * 
 * @return {Array.<String>} an array of parameter strings that can be passed
 *   to the binary.
 */
MongoRunner.arrOptions = function( binaryName , args ){

    var fullArgs = [ binaryName ]

    if ( isObject( args ) || ( args.length == 1 && isObject( args[0] ) ) ){

        var o = isObject( args ) ? args : args[0]
        for ( var k in o ){
            
            if( ! o.hasOwnProperty(k) || k in MongoRunner.logicalOptions ) continue
            
            if ( ( k == "v" || k == "verbose" ) && isNumber( o[k] ) ){
                var n = o[k]
                if ( n > 0 ){
                    if ( n > 10 ) n = 10
                    var temp = "-"
                    while ( n-- > 0 ) temp += "v"
                    fullArgs.push( temp )
                }
            }
            else {
                if( o[k] == undefined || o[k] == null ) continue
                fullArgs.push( "--" + k )
                if ( o[k] != "" )
                    fullArgs.push( "" + o[k] ) 
            }
        } 
    }
    else {
        for ( var i=0; i<args.length; i++ )
            fullArgs.push( args[i] )
    }

    return fullArgs
}

MongoRunner.arrToOpts = function( arr ){
        
    var opts = {}
    for( var i = 1; i < arr.length; i++ ){
        if( arr[i].startsWith( "-" ) ){
            var opt = arr[i].replace( /^-/, "" ).replace( /^-/, "" )
            
            if( arr.length > i + 1 && ! arr[ i + 1 ].startsWith( "-" ) ){
                opts[ opt ] = arr[ i + 1 ]
                i++
            }
            else{
                opts[ opt ] = ""
            }
            
            if( opt.replace( /v/g, "" ) == "" ){
                opts[ "verbose" ] = opt.length
            }
        }
    }
    
    return opts
}

MongoRunner.savedOptions = {}

MongoRunner.mongoOptions = function( opts ){
    
    // Initialize and create a copy of the opts
    opts = Object.merge( opts || {}, {} )
    
    if( ! opts.restart ) opts.restart = false
    
    // RunId can come from a number of places
    if( isObject( opts.restart ) ){
        opts.runId = opts.restart
        opts.restart = true
    }
    
    if( isObject( opts.remember ) ){
        opts.runId = opts.remember
        opts.remember = true
    }
    else if( opts.remember == undefined ){
        // Remember by default if we're restarting
        opts.remember = opts.restart
    }
    
    // If we passed in restart : <conn> or runId : <conn>
    if( isObject( opts.runId ) && opts.runId.runId ) opts.runId = opts.runId.runId
    
    if( opts.restart && opts.remember ) opts = Object.merge( MongoRunner.savedOptions[ opts.runId ], opts )

    // Create a new runId
    opts.runId = opts.runId || ObjectId()
    
    // Save the port if required
    if( ! opts.forgetPort ) opts.port = opts.port || MongoRunner.nextOpenPort()
    
    var shouldRemember = ( ! opts.restart && ! opts.noRemember ) || ( opts.restart && opts.appendOptions )
    
    if ( shouldRemember ){
        MongoRunner.savedOptions[ opts.runId ] = Object.merge( opts, {} )
    }
    
    opts.port = opts.port || MongoRunner.nextOpenPort()
    MongoRunner.usedPortMap[ "" + parseInt( opts.port ) ] = true
    
    opts.pathOpts = Object.merge( opts.pathOpts || {}, { port : "" + opts.port, runId : "" + opts.runId } )
    
    return opts
}

/**
 * @option {object} opts
 * 
 *   {
 *     dbpath {string}
 *     useLogFiles {boolean}: use with logFile option.
 *     logFile {string}: path to the log file. If not specified and useLogFiles
 *       is true, automatically creates a log file inside dbpath.
 *     noJournalPrealloc {boolean}
 *     noJournal {boolean}
 *     keyFile
 *     replSet
 *     oplogSize
 *   }
 */
MongoRunner.mongodOptions = function( opts ){
    
    opts = MongoRunner.mongoOptions( opts )
    
    opts.dbpath = MongoRunner.toRealDir( opts.dbpath || "$dataDir/mongod-$port",
                                         opts.pathOpts )
                                         
    opts.pathOpts = Object.merge( opts.pathOpts, { dbpath : opts.dbpath } )
    
    if( ! opts.logFile && opts.useLogFiles ){
        opts.logFile = opts.dbpath + "/mongod.log"
    }
    else if( opts.logFile ){
        opts.logFile = MongoRunner.toRealFile( opts.logFile, opts.pathOpts )
    }
    
    if( jsTestOptions().noJournalPrealloc || opts.noJournalPrealloc )
        opts.nopreallocj = ""
            
    if( jsTestOptions().noJournal || opts.noJournal )
        opts.nojournal = ""

    if( jsTestOptions().keyFile && !opts.keyFile) {
        opts.keyFile = jsTestOptions().keyFile
    }

    if( opts.noReplSet ) opts.replSet = null
    if( opts.arbiter ) opts.oplogSize = 1
            
    return opts
}

MongoRunner.mongosOptions = function( opts ){
    
    opts = MongoRunner.mongoOptions( opts )
    
    opts.pathOpts = Object.merge( opts.pathOpts, 
                                { configdb : opts.configdb.replace( /:|,/g, "-" ) } )
    
    if( ! opts.logFile && opts.useLogFiles ){
        opts.logFile = MongoRunner.toRealFile( "$dataDir/mongos-$configdb-$port.log",
                                               opts.pathOpts )
    }
    else if( opts.logFile ){
        opts.logFile = MongoRunner.toRealFile( opts.logFile, opts.pathOpts )
    }

    if( jsTestOptions().keyFile && !opts.keyFile) {
        opts.keyFile = jsTestOptions().keyFile
    }

    return opts
}

/**
 * Starts a mongod instance.
 * 
 * @param {Object} opts
 * 
 *   {
 *     useHostName {boolean}: Uses hostname of machine if true
 *     forceLock {boolean}: Deletes the lock file if set to true
 *     dbpath {string}: location of db files
 *     cleanData {boolean}: Removes all files in dbpath if true
 *     startClean {boolean}: same as cleanData
 *     noCleanData {boolean}: Do not clean files (cleanData takes priority)
 * 
 *     @see MongoRunner.mongodOptions for other options
 *   }
 * 
 * @return {Mongo} connection object to the started mongod instance.
 * 
 * @see MongoRunner.arrOptions
 */
MongoRunner.runMongod = function( opts ){
    
    var useHostName = false
    var runId = null
    if( isObject( opts ) ) {
        
        opts = MongoRunner.mongodOptions( opts )
        
        useHostName = opts.useHostName || opts.useHostname
        runId = opts.runId
        
        if( opts.forceLock ) removeFile( opts.dbpath + "/mongod.lock" )
        if( ( opts.cleanData || opts.startClean ) || ( ! opts.restart && ! opts.noCleanData ) ){
            print( "Resetting db path '" + opts.dbpath + "'" )
            resetDbpath( opts.dbpath )
        }
        
        opts = MongoRunner.arrOptions( "mongod", opts )
    }
    
    var mongod = startMongoProgram.apply( null, opts )
    mongod.commandLine = MongoRunner.arrToOpts( opts )
    mongod.name = (useHostName ? getHostName() : "localhost") + ":" + mongod.commandLine.port
    mongod.host = mongod.name
    mongod.port = parseInt( mongod.commandLine.port )
    mongod.runId = runId || ObjectId()
    mongod.savedOptions = MongoRunner.savedOptions[ mongod.runId ]
    
    return mongod
}

MongoRunner.runMongos = function( opts ){
    
    var useHostName = false
    var runId = null
    if( isObject( opts ) ) {
        
        opts = MongoRunner.mongosOptions( opts )
        
        useHostName = opts.useHostName || opts.useHostname
        runId = opts.runId
        
        opts = MongoRunner.arrOptions( "mongos", opts )
    }
    
    var mongos = startMongoProgram.apply( null, opts )
    mongos.commandLine = MongoRunner.arrToOpts( opts )
    mongos.name = (useHostName ? getHostName() : "localhost") + ":" + mongos.commandLine.port
    mongos.host = mongos.name
    mongos.port = parseInt( mongos.commandLine.port ) 
    mongos.runId = runId || ObjectId()
    mongos.savedOptions = MongoRunner.savedOptions[ mongos.runId ]
        
    return mongos
}

/**
 * Kills a mongod process.
 *
 * @param {number} port the port of the process to kill
 * @param {number} signal The signal number to use for killing
 * @param {Object} opts Additional options. Format:
 *    {
 *      auth: {
 *        user {string}: admin user name
 *        pwd {string}: admin password
 *      }
 *    }
 *
 * Note: The auth option is required in a authenticated mongod running in Windows since
 *  it uses the shutdown command, which requires admin credentials.
 */
MongoRunner.stopMongod = function( port, signal, opts ){
    
    if( ! port ) {
        print( "Cannot stop mongo process " + port )
        return
    }
    
    signal = signal || 15
    
    if( port.port )
        port = parseInt( port.port )
    
    if( port instanceof ObjectId ){
        var opts = MongoRunner.savedOptions( port )
        if( opts ) port = parseInt( opts.port )
    }
    
    var exitCode = stopMongod( parseInt( port ), parseInt( signal ), opts )
    
    delete MongoRunner.usedPortMap[ "" + parseInt( port ) ]

    return exitCode
}

MongoRunner.stopMongos = MongoRunner.stopMongod

MongoRunner.isStopped = function( port ){
    
    if( ! port ) {
        print( "Cannot detect if process " + port + " is stopped." )
        return
    }
    
    if( port.port )
        port = parseInt( port.port )
    
    if( port instanceof ObjectId ){
        var opts = MongoRunner.savedOptions( port )
        if( opts ) port = parseInt( opts.port )
    }
    
    return MongoRunner.usedPortMap[ "" + parseInt( port ) ] ? false : true
}

__nextPort = 27000;
startMongodTest = function (port, dirname, restart, extraOptions ) {
    if (!port)
        port = __nextPort++;
    var f = startMongodEmpty;
    if (restart)
        f = startMongodNoReset;
    if (!dirname)
        dirname = "" + port; // e.g., data/db/27000

    var useHostname = false;
    if (extraOptions) {
         useHostname = extraOptions.useHostname;
         delete extraOptions.useHostname;
    }

    
    var options = 
        {
            port: port,
            dbpath: "/data/db/" + dirname,
            noprealloc: "",
            smallfiles: "",
            oplogSize: "40",
            nohttpinterface: ""
        };
    
    if( jsTestOptions().noJournal ) options["nojournal"] = ""
    if( jsTestOptions().noJournalPrealloc ) options["nopreallocj"] = ""
    if( jsTestOptions().auth ) options["auth"] = ""
    if( jsTestOptions().keyFile && (!extraOptions || !extraOptions['keyFile']) ) options['keyFile'] = jsTestOptions().keyFile

    if ( extraOptions )
        Object.extend( options , extraOptions );
    
    var conn = f.apply(null, [ options ] );

    conn.name = (useHostname ? getHostName() : "localhost") + ":" + port;

    if (options['auth'] || options['keyFile']) {
        if (!this.shardsvr && !options.replSet) {
            jsTest.addAuth(conn);
        }
        jsTest.authenticate(conn);
    }
    return conn;
}

// Start a mongod instance and return a 'Mongo' object connected to it.
// This function's arguments are passed as command line arguments to mongod.
// The specified 'dbpath' is cleared if it exists, created if not.
// var conn = startMongodEmpty("--port", 30000, "--dbpath", "asdf");
startMongodEmpty = function () {
    var args = createMongoArgs("mongod", arguments);

    var dbpath = _parsePath.apply(null, args);
    resetDbpath(dbpath);

    return startMongoProgram.apply(null, args);
}
startMongod = function () {
    print("startMongod WARNING DELETES DATA DIRECTORY THIS IS FOR TESTING ONLY");
    return startMongodEmpty.apply(null, arguments);
}
startMongodNoReset = function(){
    var args = createMongoArgs( "mongod" , arguments );
    return startMongoProgram.apply( null, args );
}

startMongos = function(args){
    return MongoRunner.runMongos(args);
}

/* Start mongod or mongos and return a Mongo() object connected to there.
  This function's first argument is "mongod" or "mongos" program name, \
  and subsequent arguments to this function are passed as
  command line arguments to the program.
*/
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
    }, "unable to connect to mongo program on port " + port, 600 * 1000 );

    return m;
}

// Start a mongo program instance.  This function's first argument is the
// program name, and subsequent arguments to this function are passed as
// command line arguments to the program.  Returns pid of the spawned program.
startMongoProgramNoConnect = function() {
    return _startMongoProgram.apply( null, arguments );
}

myPort = function() {
    var m = db.getMongo();
    if ( m.host.match( /:/ ) )
        return m.host.match( /:(.*)/ )[ 1 ];
    else
        return 27017;
}
