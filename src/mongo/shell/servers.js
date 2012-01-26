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
        path = path.replace( RegExp( "\\$" + key, "g" ), pathOpts[ key ] )
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

MongoRunner.stopMongod = function( port, signal ){
    
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
    
    var exitCode = stopMongod( parseInt( port ), parseInt( signal ) )
    
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

/**
 * otherParams can be:
 * * useHostname to use the hostname (instead of localhost)
 */
ShardingTest = function( testName , numShards , verboseLevel , numMongos , otherParams ){
    
    this._startTime = new Date();

    // Check if testName is an object, if so, pull params from there
    var keyFile = undefined
    otherParams = Object.merge( otherParams || {}, {} )
    otherParams.extraOptions = otherParams.extraOptions || {}
    
    if( isObject( testName ) ){
        
        var params = Object.merge( testName, {} )
        
        testName = params.name || "test"
        
        otherParams = Object.merge( params.other || {}, {} )
        otherParams.extraOptions = otherParams.extraOptions || {}
        
        numShards = params.shards || 2
        verboseLevel = params.verbose || 0
        numMongos = params.mongos || 1
        
        keyFile = params.keyFile || otherParams.keyFile || otherParams.extraOptions.keyFile
        otherParams.nopreallocj = params.nopreallocj || otherParams.nopreallocj
        otherParams.rs = params.rs || ( params.other ? params.other.rs : undefined )
        otherParams.chunksize = params.chunksize || ( params.other ? params.other.chunksize : undefined )
        
        // Allow specifying options like :
        // { mongos : [ { noprealloc : "" } ], config : [ { smallfiles : "" } ], shards : { rs : true, d : true } } 
        if( isObject( numShards ) ){
            var len = 0
            for( var i in numShards ){
                otherParams[ "" + i ] = numShards[i]
                len++
            }
            numShards = len
        }
        
        if( isObject( numMongos ) ){
            var len = 0
            for( var i in numMongos ){
                otherParams[ "" + i ] = numMongos[i]
                len++
            }
            numMongos = len
        }
        else if( Array.isArray( numMongos ) ){
            for( var i = 0; i < numMongos.length; i++ )
                otherParams[ "s" + i ] = numMongos[i]
            numMongos = numMongos.length
        }
        
        if( isObject( params.config ) ){
            var len = 0
            for( var i in params.config ){
                otherParams[ "" + i ] = params.config[i]
                len++
            }
            
            // If we're specifying explicit config options, we need separate config servers
            otherParams.separateConfig = true
            if( len == 3 ) otherParams.sync = true
            else otherParams.sync = false
        }
        else if( Array.isArray( params.config ) ){
            for( var i = 0; i < params.config.length; i++ )
                otherParams[ "c" + i ] = params.config[i]
            
            // If we're specifying explicit config options, we need separate config servers
            otherParams.separateConfig = true
            if( params.config.length == 3 ) otherParams.sync = true
            else otherParams.sync = false
        }
        else if( params.config ) {
            
            if( params.config == 3 ){
                otherParams.separateConfig = otherParams.separateConfig || true
                otherParams.sync = true
            }
            
        }
    }
    else {
        // Handle legacy stuff
        keyFile = otherParams.extraOptions.keyFile
    }

    this._testName = testName
    this._otherParams = otherParams
    
    var pathOpts = this.pathOpts = { testName : testName }

    var hasRS = false
    for( var k in otherParams ){
        if( k.startsWith( "rs" ) ){
            hasRS = true
            break
        }
    }
    
    if( hasRS ){
        otherParams.separateConfig = true
        otherParams.useHostname = otherParams.useHostname == undefined ? true : otherParams.useHostname
    }
    
    var localhost = otherParams.useHostname ? getHostName() : "localhost";

    this._alldbpaths = []
    this._connections = []
    this._shardServers = this._connections
    this._rs = []
    this._rsObjects = []

    for ( var i = 0; i < numShards; i++ ) {
        if( otherParams.rs || otherParams["rs" + i] ){
            
            otherParams.separateConfig = true
            
            var setName = testName + "-rs" + i;
            
            rsDefaults = { useHostname : otherParams.useHostname,
                           noJournalPrealloc : otherParams.nopreallocj, 
                           oplogSize : 40,
                           nodes : 3,
                           pathOpts : Object.merge( pathOpts, { shard : i } )}
            
            rsDefaults = Object.merge( rsDefaults, otherParams.rs )
            rsDefaults = Object.merge( rsDefaults, otherParams.rsOptions )
            rsDefaults = Object.merge( rsDefaults, otherParams["rs" + i] )
            
            var numReplicas = rsDefaults.nodes || otherParams.numReplicas || 3
            delete rsDefaults.nodes
            
            print( "Replica set test!" )
            
            var rs = new ReplSetTest( { name : setName , nodes : numReplicas , startPort : 31100 + ( i * 100 ), useHostName : otherParams.useHostname, keyFile : keyFile, shardSvr : true } );
            this._rs[i] = { setName : setName , test : rs , nodes : rs.startSet( rsDefaults ) , url : rs.getURL() };
            rs.initiate();
            this["rs" + i] = rs
            
            this._rsObjects[i] = rs
            
            this._alldbpaths.push( null )
            this._connections.push( null )
        }
        else {
            var options = { useHostname : otherParams.useHostname,
                            noJournalPrealloc : otherParams.nopreallocj,
                            port : 30000 + i,
                            pathOpts : Object.merge( pathOpts, { shard : i } ),
                            dbpath : "$testName$shard",
                            keyFile : keyFile
                          }
            
            options = Object.merge( options, otherParams.shardOptions )
            options = Object.merge( options, otherParams["d" + i] )
            
            var conn = MongoRunner.runMongod( options );
            
            this._alldbpaths.push( testName +i )
            this._connections.push( conn );
            this["shard" + i] = conn
            this["d" + i] = conn
            
            this._rs[i] = null
            this._rsObjects[i] = null
        }
    }
        
    // Do replication on replica sets if required
    for ( var i = 0; i < numShards; i++ ){
        if( ! otherParams.rs && ! otherParams["rs" + i] ) continue
        
        var rs = this._rs[i].test;
        
        rs.getMaster().getDB( "admin" ).foo.save( { x : 1 } )
        rs.awaitReplication();
        
        var rsConn = new Mongo( rs.getURL() );
        rsConn.name = rs.getURL();
        this._connections[i] = rsConn
        this["shard" + i] = rsConn
        rsConn.rs = rs
    }

    this._configServers = []
    this._configNames = []
    
    if ( otherParams.sync && ! otherParams.separateConfig && numShards < 3 )
        throw "if you want sync, you need at least 3 servers";
    
    for ( var i = 0; i < ( otherParams.sync ? 3 : 1 ) ; i++ ) {
        
        var conn = null
        
        if( otherParams.separateConfig ){
            
            var options = { useHostname : otherParams.useHostname, 
                            noJournalPrealloc : otherParams.nopreallocj, 
                            port : 29000 + i,
                            pathOpts : Object.merge( pathOpts, { config : i } ),
                            dbpath : "$testName-config$config",
                            keyFile : keyFile
                          }
            
            options = Object.merge( options, otherParams.configOptions )
            options = Object.merge( options, otherParams["c" + i] )
                        
            var conn = MongoRunner.runMongod( options )
            
            // TODO:  Needed?
            this._alldbpaths.push( testName + "-config" + i )
        }
        else{
            conn = this["shard" + i]
        }
        
        this._configServers.push( conn );
        this._configNames.push( conn.name )
        this["config" + i] = conn
        this["c" + i] = conn
    }

    printjson( this._configDB = this._configNames.join( "," ) )
    this._configConnection = new Mongo( this._configDB )
    if ( ! otherParams.noChunkSize ) {
        this._configConnection.getDB( "config" ).settings.insert( { _id : "chunksize" , value : otherParams.chunksize || otherParams.chunkSize || 50 } )
    }

    print( "ShardingTest " + this._testName + " :\n" + tojson( { config : this._configDB, shards : this._connections } ) );
    
    this._mongos = []
    this._mongoses = this._mongos
    for ( var i = 0; i < ( ( numMongos == 0 ? -1 : numMongos ) || 1 ); i++ ){
        
        var options = { useHostname : otherParams.useHostname, 
                        port : 31000 - i - 1,
                        pathOpts : Object.merge( pathOpts, { mongos : i } ),
                        configdb : this._configDB,
                        verbose : verboseLevel || 0,
                        keyFile : keyFile
                      }

        options = Object.merge( options, otherParams.mongosOptions )
        options = Object.merge( options, otherParams.extraOptions )
        options = Object.merge( options, otherParams["s" + i] )
        
        var conn = MongoRunner.runMongos( options )

        this._mongos.push( conn );
        if ( i == 0 ) this.s = conn
        this["s" + i] = conn
    }

    var admin = this.admin = this.s.getDB( "admin" );
    this.config = this.s.getDB( "config" );

    if ( ! otherParams.manualAddShard ){
        this._shardNames = []
        var shardNames = this._shardNames
        this._connections.forEach(
            function(z){
                var n = z.name;
                if ( ! n ){
                    n = z.host;
                    if ( ! n )
                        n = z;
                }
                print( "ShardingTest " + this._testName + " going to add shard : " + n )
                x = admin.runCommand( { addshard : n } );
                printjson( x )
                shardNames.push( x.shardAdded )
                z.shardName = x.shardAdded
            }
        );
    }

    if (jsTestOptions().keyFile && !keyFile) {
        jsTest.addAuth(this._mongos[0]);
        jsTest.authenticateNodes(this._connections);
        jsTest.authenticateNodes(this._configServers);
        jsTest.authenticateNodes(this._mongos);
    }
}

ShardingTest.prototype.getRSEntry = function( setName ){
    for ( var i=0; i<this._rs.length; i++ )
        if ( this._rs[i].setName == setName )
            return this._rs[i];
    throw "can't find rs: " + setName;
}

ShardingTest.prototype.getConfigIndex = function( config ){
    
    // Assume config is a # if not a conn object
    if( ! isObject( config ) ) config = getHostName() + ":" + config
    
    for( var i = 0; i < this._configServers.length; i++ ){
        if( connectionURLTheSame( this._configServers[i], config ) ) return i
    }
    
    return -1
}

ShardingTest.prototype.getDB = function( name ){
    return this.s.getDB( name );
}

ShardingTest.prototype.getServerName = function( dbname ){
    var x = this.config.databases.findOne( { _id : "" + dbname } );
    if ( x )
        return x.primary;
    this.config.databases.find().forEach( printjson );
    throw "couldn't find dbname: " + dbname + " total: " + this.config.databases.count();
}


ShardingTest.prototype.getNonPrimaries = function( dbname ){
    var x = this.config.databases.findOne( { _id : dbname } );
    if ( ! x ){
        this.config.databases.find().forEach( printjson );
        throw "couldn't find dbname: " + dbname + " total: " + this.config.databases.count();
    }
    
    return this.config.shards.find( { _id : { $ne : x.primary } } ).map( function(z){ return z._id; } )
}


ShardingTest.prototype.getConnNames = function(){
    var names = [];
    for ( var i=0; i<this._connections.length; i++ ){
        names.push( this._connections[i].name );
    }
    return names; 
}

ShardingTest.prototype.getServer = function( dbname ){
    var name = this.getServerName( dbname );

    var x = this.config.shards.findOne( { _id : name } );
    if ( x )
        name = x.host;

    var rsName = null;
    if ( name.indexOf( "/" ) > 0 )
	rsName = name.substring( 0 , name.indexOf( "/" ) );
    
    for ( var i=0; i<this._connections.length; i++ ){
        var c = this._connections[i];
        if ( connectionURLTheSame( name , c.name ) || 
             connectionURLTheSame( rsName , c.name ) )
            return c;
    }
    
    throw "can't find server for: " + dbname + " name:" + name;

}

ShardingTest.prototype.normalize = function( x ){
    var z = this.config.shards.findOne( { host : x } );
    if ( z )
        return z._id;
    return x;
}

ShardingTest.prototype.getOther = function( one ){
    if ( this._connections.length < 2 )
        throw "getOther only works with 2 servers";

    if ( one._mongo )
        one = one._mongo
    
    for( var i = 0; i < this._connections.length; i++ ){
        if( this._connections[i] != one ) return this._connections[i]
    }
    
    return null
}

ShardingTest.prototype.getAnother = function( one ){
    if(this._connections.length < 2)
    	throw "getAnother() only works with multiple servers";
	
	if ( one._mongo )
        one = one._mongo
    
    for(var i = 0; i < this._connections.length; i++){
    	if(this._connections[i] == one)
    		return this._connections[(i + 1) % this._connections.length];
    }
}

ShardingTest.prototype.getFirstOther = function( one ){
    for ( var i=0; i<this._connections.length; i++ ){
        if ( this._connections[i] != one )
        return this._connections[i];
    }
    throw "impossible";
}

ShardingTest.prototype.stop = function(){
    for ( var i=0; i<this._mongos.length; i++ ){
        stopMongoProgram( 31000 - i - 1 );
    }
    for ( var i=0; i<this._connections.length; i++){
        stopMongod( 30000 + i );
    }
    if ( this._rs ){
        for ( var i=0; i<this._rs.length; i++ ){
            if( this._rs[i] ) this._rs[i].test.stopSet( 15 );
        }
    }
    if( this._otherParams.separateConfig ){
        for ( var i=0; i<this._configServers.length; i++ ){
            MongoRunner.stopMongod( this._configServers[i] )
        }
    }
    if ( this._alldbpaths ){
        for( i=0; i<this._alldbpaths.length; i++ ){
            resetDbpath( "/data/db/" + this._alldbpaths[i] );
        }
    }

    var timeMillis = new Date().getTime() - this._startTime.getTime();

    print('*** ShardingTest ' + this._testName + " completed successfully in " + ( timeMillis / 1000 ) + " seconds ***");
}

ShardingTest.prototype.adminCommand = function(cmd){
    var res = this.admin.runCommand( cmd );
    if ( res && res.ok == 1 )
        return true;

    throw "command " + tojson( cmd ) + " failed: " + tojson( res );
}

ShardingTest.prototype._rangeToString = function(r){
    return tojsononeline( r.min ) + " -> " + tojsononeline( r.max );
}

ShardingTest.prototype.printChangeLog = function(){
    var s = this;
    this.config.changelog.find().forEach( 
        function(z){
            var msg = z.server + "\t" + z.time + "\t" + z.what;
            for ( i=z.what.length; i<15; i++ )
                msg += " ";
            msg += " " + z.ns + "\t";
            if ( z.what == "split" ){
                msg += s._rangeToString( z.details.before ) + " -->> (" + s._rangeToString( z.details.left ) + "),(" + s._rangeToString( z.details.right ) + ")";
            }
            else if (z.what == "multi-split" ){
                msg += s._rangeToString( z.details.before ) + "  -->> (" + z.details.number + "/" + z.details.of + " " + s._rangeToString( z.details.chunk ) + ")"; 
            }
            else {
                msg += tojsononeline( z.details );
            }

            print( "ShardingTest " + msg )
        }
    );

}

ShardingTest.prototype.getChunksString = function( ns ){
    var q = {}
    if ( ns )
        q.ns = ns;

    var s = "";
    this.config.chunks.find( q ).sort( { ns : 1 , min : 1 } ).forEach( 
        function(z){
            s +=  "  " + z._id + "\t" + z.lastmod.t + "|" + z.lastmod.i + "\t" + tojson(z.min) + " -> " + tojson(z.max) + " " + z.shard + "  " + z.ns + "\n";
        }
    );
    
    return s;
}

ShardingTest.prototype.printChunks = function( ns ){
    print( "ShardingTest " + this.getChunksString( ns ) );
}

ShardingTest.prototype.printShardingStatus = function(){
    printShardingStatus( this.config );
}

ShardingTest.prototype.printCollectionInfo = function( ns , msg ){
    var out = "";
    if ( msg )
        out += msg + "\n";
    out += "sharding collection info: " + ns + "\n";
    for ( var i=0; i<this._connections.length; i++ ){
        var c = this._connections[i];
        out += "  mongod " + c + " " + tojson( c.getCollection( ns ).getShardVersion() , " " , true ) + "\n";
    }
    for ( var i=0; i<this._mongos.length; i++ ){
        var c = this._mongos[i];
        out += "  mongos " + c + " " + tojson( c.getCollection( ns ).getShardVersion() , " " , true ) + "\n";
    }
    
    out += this.getChunksString( ns );

    print( "ShardingTest " + out );
}

printShardingStatus = function( configDB , verbose ){
    if (configDB === undefined)
        configDB = db.getSisterDB('config')
    
    var version = configDB.getCollection( "version" ).findOne();
    if ( version == null ){
        print( "printShardingStatus: this db does not have sharding enabled. be sure you are connecting to a mongos from the shell and not to a mongod." );
        return;
    }
    
    var raw = "";
    var output = function(s){
        raw += s + "\n";
    }
    output( "--- Sharding Status --- " );
    output( "  sharding version: " + tojson( configDB.getCollection( "version" ).findOne() ) );
    
    output( "  shards:" );
    configDB.shards.find().sort( { _id : 1 } ).forEach( 
        function(z){
            output( "\t" + tojsononeline( z ) );
        }
    );

    output( "  databases:" );
    configDB.databases.find().sort( { name : 1 } ).forEach( 
        function(db){
            output( "\t" + tojsononeline(db,"",true) );
        
            if (db.partitioned){
                configDB.collections.find( { _id : new RegExp( "^" + db._id + "\\." ) } ).sort( { _id : 1 } ).forEach(
                    function( coll ){
                        if ( coll.dropped == false ){
                            output("\t\t" + coll._id + " chunks:");
                            
                            res = configDB.chunks.group( { cond : { ns : coll._id } , key : { shard : 1 }  , reduce : function( doc , out ){ out.nChunks++; } , initial : { nChunks : 0 } } );
                            var totalChunks = 0;
                            res.forEach( function(z){
                                totalChunks += z.nChunks;
                                output( "\t\t\t\t" + z.shard + "\t" + z.nChunks );
                            } )
                            
                            if ( totalChunks < 20 || verbose ){
                                configDB.chunks.find( { "ns" : coll._id } ).sort( { min : 1 } ).forEach( 
                                    function(chunk){
                                        output( "\t\t\t" + tojson( chunk.min ) + " -->> " + tojson( chunk.max ) + 
                                                " on : " + chunk.shard + " " + tojson( chunk.lastmod ) + " " +
                                                ( chunk.jumbo ? "jumbo " : "" ) );
                                    }
                                );
                            }
                            else {
                                output( "\t\t\ttoo many chunks to print, use verbose if you want to force print" );
                            }
                        }
                    }
                )
            }
        }
    );
    
    print( raw );
}

printShardingSizes = function(){
    configDB = db.getSisterDB('config')
    
    var version = configDB.getCollection( "version" ).findOne();
    if ( version == null ){
        print( "printShardingSizes : not a shard db!" );
        return;
    }
    
    var raw = "";
    var output = function(s){
        raw += s + "\n";
    }
    output( "--- Sharding Status --- " );
    output( "  sharding version: " + tojson( configDB.getCollection( "version" ).findOne() ) );
    
    output( "  shards:" );
    var shards = {};
    configDB.shards.find().forEach( 
        function(z){
            shards[z._id] = new Mongo(z.host);
            output( "      " + tojson(z) );
        }
    );

    var saveDB = db;
    output( "  databases:" );
    configDB.databases.find().sort( { name : 1 } ).forEach( 
        function(db){
            output( "\t" + tojson(db,"",true) );
        
            if (db.partitioned){
                configDB.collections.find( { _id : new RegExp( "^" + db._id + "\." ) } ).sort( { _id : 1 } ).forEach(
                    function( coll ){
                        output("\t\t" + coll._id + " chunks:");
                        configDB.chunks.find( { "ns" : coll._id } ).sort( { min : 1 } ).forEach( 
                            function(chunk){
                                var mydb = shards[chunk.shard].getDB(db._id)
                                var out = mydb.runCommand({dataSize: coll._id,
                                                           keyPattern: coll.key, 
                                                           min: chunk.min,
                                                           max: chunk.max });
                                delete out.millis;
                                delete out.ok;

                                output( "\t\t\t" + tojson( chunk.min ) + " -->> " + tojson( chunk.max ) + 
                                        " on : " + chunk.shard + " " + tojson( out ) );

                            }
                        );
                    }
                )
            }
        }
    );
    
    print( raw );
}

ShardingTest.prototype.sync = function(){
    this.adminCommand( "connpoolsync" );
}

ShardingTest.prototype.onNumShards = function( collName , dbName ){
    this.sync(); // we should sync since we're going directly to mongod here
    dbName = dbName || "test";
    var num=0;
    for ( var i=0; i<this._connections.length; i++ )
        if ( this._connections[i].getDB( dbName ).getCollection( collName ).count() > 0 )
            num++;
    return num;
}


ShardingTest.prototype.shardCounts = function( collName , dbName ){
    this.sync(); // we should sync since we're going directly to mongod here
    dbName = dbName || "test";
    var counts = {}
    for ( var i=0; i<this._connections.length; i++ )
        counts[i] = this._connections[i].getDB( dbName ).getCollection( collName ).count();
    return counts;
}

ShardingTest.prototype.chunkCounts = function( collName , dbName ){
    dbName = dbName || "test";
    var x = {}

    s.config.shards.find().forEach( 
        function(z){
            x[z._id] = 0;
        }
    );
    
    s.config.chunks.find( { ns : dbName + "." + collName } ).forEach(
        function(z){
            if ( x[z.shard] )
                x[z.shard]++
            else
                x[z.shard] = 1;
        }
    );
    return x;

}

ShardingTest.prototype.chunkDiff = function( collName , dbName ){
    var c = this.chunkCounts( collName , dbName );
    var min = 100000000;
    var max = 0;
    for ( var s in c ){
        if ( c[s] < min )
            min = c[s];
        if ( c[s] > max )
            max = c[s];
    }
    print( "ShardingTest input: " + tojson( c ) + " min: " + min + " max: " + max  );
    return max - min;
}

ShardingTest.prototype.getShard = function( coll, query, includeEmpty ){
    var shards = this.getShards( coll, query, includeEmpty )
    assert.eq( shards.length, 1 )
    return shards[0]
}

// Returns the shards on which documents matching a particular query reside
ShardingTest.prototype.getShards = function( coll, query, includeEmpty ){
    if( ! coll.getDB )
        coll = this.s.getCollection( coll )
    
    var explain = coll.find( query ).explain()
    var shards = []
        
    if( explain.shards ){
        
        for( var shardName in explain.shards ){           
            for( var i = 0; i < explain.shards[shardName].length; i++ ){
                if( includeEmpty || ( explain.shards[shardName][i].n && explain.shards[shardName][i].n > 0 ) )
                    shards.push( shardName )
            }
        }
        
    }
    
    for( var i = 0; i < shards.length; i++ ){
        for( var j = 0; j < this._connections.length; j++ ){
            if ( connectionURLTheSame(  this._connections[j] , shards[i] ) ){
                shards[i] = this._connections[j]
                break;
            }
        }
    }
    
    return shards
}

ShardingTest.prototype.isSharded = function( collName ){
    
    var collName = "" + collName
    var dbName = undefined
    
    if( typeof collName.getCollectionNames == 'function' ){
        dbName = "" + collName
        collName = undefined
    }
    
    if( dbName ){
        var x = this.config.databases.findOne( { _id : dbname } )
        if( x ) return x.partitioned
        else return false
    }
    
    if( collName ){
        var x = this.config.collections.findOne( { _id : collName } )
        if( x ) return true
        else return false
    }
    
}

ShardingTest.prototype.shardGo = function( collName , key , split , move , dbName ){
    
    split = ( split != false ? ( split || key ) : split )
    move = ( split != false && move != false ? ( move || split ) : false )
    
    if( collName.getDB )
        dbName = "" + collName.getDB()
    else dbName = dbName || "test";

    var c = dbName + "." + collName;
    if( collName.getDB )
        c = "" + collName

    var isEmpty = this.s.getCollection( c ).count() == 0
        
    if( ! this.isSharded( dbName ) )
        this.s.adminCommand( { enableSharding : dbName } )
    
    var result = this.s.adminCommand( { shardcollection : c , key : key } )
    if( ! result.ok ){
        printjson( result )
        assert( false )
    }
    
    if( split == false ) return
    
    result = this.s.adminCommand( { split : c , middle : split } );
    if( ! result.ok ){
        printjson( result )
        assert( false )
    }
        
    if( move == false ) return
    
    var result = null
    for( var i = 0; i < 5; i++ ){
        result = this.s.adminCommand( { movechunk : c , find : move , to : this.getOther( this.getServer( dbName ) ).name } );
        if( result.ok ) break;
        sleep( 5 * 1000 );
    }
    printjson( result )
    assert( result.ok )
    
};

ShardingTest.prototype.shardColl = ShardingTest.prototype.shardGo

ShardingTest.prototype.setBalancer = function( balancer ){
    if( balancer || balancer == undefined ){
        this.config.settings.update( { _id: "balancer" }, { $set : { stopped: false } } , true )
    }
    else if( balancer == false ){
        this.config.settings.update( { _id: "balancer" }, { $set : { stopped: true } } , true )
    }
}

ShardingTest.prototype.stopBalancer = function( timeout, interval ) {
    this.setBalancer( false )
    
    if( typeof db == "undefined" ) db = undefined
    var oldDB = db
    
    db = this.config
    sh.waitForBalancer( false, timeout, interval )
    db = oldDB
}

ShardingTest.prototype.startBalancer = function( timeout, interval ) {
    this.setBalancer( true )
    
    if( typeof db == "undefined" ) db = undefined
    var oldDB = db
    
    db = this.config
    sh.waitForBalancer( true, timeout, interval )
    db = oldDB
}

/**
 * Run a mongod process.
 *
 * After initializing a MongodRunner, you must call start() on it.
 * @param {int} port port to run db on, use allocatePorts(num) to requision
 * @param {string} dbpath path to use
 * @param {boolean} peer pass in false (DEPRECATED, was used for replica pair host)
 * @param {boolean} arbiter pass in false (DEPRECATED, was used for replica pair host)
 * @param {array} extraArgs other arguments for the command line
 * @param {object} options other options include no_bind to not bind_ip to 127.0.0.1
 *    (necessary for replica set testing)
 */
MongodRunner = function( port, dbpath, peer, arbiter, extraArgs, options ) {
    this.port_ = port;
    this.dbpath_ = dbpath;
    this.peer_ = peer;
    this.arbiter_ = arbiter;
    this.extraArgs_ = extraArgs;
    this.options_ = options ? options : {};
};

/**
 * Start this mongod process.
 *
 * @param {boolean} reuseData If the data directory should be left intact (default is to wipe it)
 */
MongodRunner.prototype.start = function( reuseData ) {
    var args = [];
    if ( reuseData ) {
        args.push( "mongod" );
    }
    args.push( "--port" );
    args.push( this.port_ );
    args.push( "--dbpath" );
    args.push( this.dbpath_ );
    args.push( "--nohttpinterface" );
    args.push( "--noprealloc" );
    args.push( "--smallfiles" );
    if (!this.options_.no_bind) {
      args.push( "--bind_ip" );
      args.push( "127.0.0.1" );
    }
    if ( this.extraArgs_ ) {
        args = args.concat( this.extraArgs_ );
    }
    removeFile( this.dbpath_ + "/mongod.lock" );
    if ( reuseData ) {
        return startMongoProgram.apply( null, args );
    } else {
        return startMongod.apply( null, args );
    }
}

MongodRunner.prototype.port = function() { return this.port_; }

MongodRunner.prototype.toString = function() { return [ this.port_, this.dbpath_, this.peer_, this.arbiter_ ].toString(); }

ToolTest = function( name ){
    this.name = name;
    this.port = allocatePorts(1)[0];
    this.baseName = "jstests_tool_" + name;
    this.root = "/data/db/" + this.baseName;
    this.dbpath = this.root + "/";
    this.ext = this.root + "_external/";
    this.extFile = this.root + "_external/a";
    resetDbpath( this.dbpath );
    resetDbpath( this.ext );
}

ToolTest.prototype.startDB = function( coll ){
    assert( ! this.m , "db already running" );
 
    this.m = startMongoProgram( "mongod" , "--port", this.port , "--dbpath" , this.dbpath , "--nohttpinterface", "--noprealloc" , "--smallfiles" , "--bind_ip", "127.0.0.1" );
    this.db = this.m.getDB( this.baseName );
    if ( coll )
        return this.db.getCollection( coll );
    return this.db;
}

ToolTest.prototype.stop = function(){
    if ( ! this.m )
        return;
    stopMongod( this.port );
    this.m = null;
    this.db = null;

    print('*** ' + this.name + " completed successfully ***");
}

ToolTest.prototype.runTool = function(){
    var a = [ "mongo" + arguments[0] ];

    var hasdbpath = false;
    
    for ( var i=1; i<arguments.length; i++ ){
        a.push( arguments[i] );
        if ( arguments[i] == "--dbpath" )
            hasdbpath = true;
    }

    if ( ! hasdbpath ){
        a.push( "--host" );
        a.push( "127.0.0.1:" + this.port );
    }

    return runMongoProgram.apply( null , a );
}


ReplTest = function( name, ports ){
    this.name = name;
    this.ports = ports || allocatePorts( 2 );
}

ReplTest.prototype.getPort = function( master ){
    if ( master )
        return this.ports[ 0 ];
    return this.ports[ 1 ]
}

ReplTest.prototype.getPath = function( master ){
    var p = "/data/db/" + this.name + "-";
    if ( master )
        p += "master";
    else
        p += "slave"
    return p;
}

ReplTest.prototype.getOptions = function( master , extra , putBinaryFirst, norepl ){

    if ( ! extra )
        extra = {};

    if ( ! extra.oplogSize )
        extra.oplogSize = "40";
        
    var a = []
    if ( putBinaryFirst )
        a.push( "mongod" )
    a.push( "--nohttpinterface", "--noprealloc", "--bind_ip" , "127.0.0.1" , "--smallfiles" );

    a.push( "--port" );
    a.push( this.getPort( master ) );

    a.push( "--dbpath" );
    a.push( this.getPath( master ) );
    
    if( jsTestOptions().noJournal ) a.push( "--nojournal" )
    if( jsTestOptions().noJournalPrealloc ) a.push( "--nopreallocj" )
    if( jsTestOptions().keyFile ) {
        a.push( "--keyFile" )
        a.push( jsTestOptions().keyFile )
    }

    if ( !norepl ) {
        if ( master ){
            a.push( "--master" );
        }
        else {
            a.push( "--slave" );
            a.push( "--source" );
            a.push( "127.0.0.1:" + this.ports[0] );
        }
    }
    
    for ( var k in extra ){
        var v = extra[k];
        if( k in MongoRunner.logicalOptions ) continue
        a.push( "--" + k );
        if ( v != null )
            a.push( v );                    
    }

    return a;
}

ReplTest.prototype.start = function( master , options , restart, norepl ){
    var lockFile = this.getPath( master ) + "/mongod.lock";
    removeFile( lockFile );
    var o = this.getOptions( master , options , restart, norepl );


    if ( restart )
        return startMongoProgram.apply( null , o );
    else
        return startMongod.apply( null , o );
}

ReplTest.prototype.stop = function( master , signal ){
    if ( arguments.length == 0 ){
        this.stop( true );
        this.stop( false );
        return;
    }

    print('*** ' + this.name + " completed successfully ***");
    return stopMongod( this.getPort( master ) , signal || 15 );
}

allocatePorts = function( n , startPort ) {
    var ret = [];
    var start = startPort || 31000;
    for( var i = start; i < start + n; ++i )
        ret.push( i );
    return ret;
}


SyncCCTest = function( testName , extraMongodOptions ){
    this._testName = testName;
    this._connections = [];
    
    for ( var i=0; i<3; i++ ){
        this._connections.push( startMongodTest( 30000 + i , testName + i , false, extraMongodOptions ) );
    }
    
    this.url = this._connections.map( function(z){ return z.name; } ).join( "," );
    this.conn = new Mongo( this.url );
}

SyncCCTest.prototype.stop = function(){
    for ( var i=0; i<this._connections.length; i++){
        stopMongod( 30000 + i );
    }

    print('*** ' + this._testName + " completed successfully ***");
}

SyncCCTest.prototype.checkHashes = function( dbname , msg ){
    var hashes = this._connections.map(
        function(z){
            return z.getDB( dbname ).runCommand( "dbhash" );
        }
    );

    for ( var i=1; i<hashes.length; i++ ){
        assert.eq( hashes[0].md5 , hashes[i].md5 , "checkHash on " + dbname + " " + msg + "\n" + tojson( hashes ) )
    }
}

SyncCCTest.prototype.tempKill = function( num ){
    num = num || 0;
    stopMongod( 30000 + num );
}

SyncCCTest.prototype.tempStart = function( num ){
    num = num || 0;
    this._connections[num] = startMongodTest( 30000 + num , this._testName + num , true );
}


function startParallelShell( jsCode, port ){
    var x;

    var args = ["mongo"];
    if (port) {
        args.push("--port", port);
    }

    if (TestData) {
        jsCode = "TestData = " + tojson(TestData) + ";jsTest.authenticate(db.getMongo());" + jsCode;
    }

    args.push("--eval", jsCode);

    if (typeof db == "object") {
        args.push(db.getMongo().host);
    }

    x = startMongoProgramNoConnect.apply(null, args);
    return function(){
        waitProgram( x );
    };
}

var testingReplication = false;

function skipIfTestingReplication(){
    if (testingReplication) {
        print("skipIfTestingReplication skipping");
        quit(0);
    }
}

ReplSetTest = function( opts ){
    this.name  = opts.name || "testReplSet";
    this.host  = opts.host || getHostName();
    this.useHostName = opts.useHostName
    this.numNodes = opts.nodes || 0;
    this.oplogSize = opts.oplogSize || 40;
    this.useSeedList = opts.useSeedList || false;
    this.bridged = opts.bridged || false;
    this.ports = [];
    this.keyFile = opts.keyFile
    this.shardSvr = opts.shardSvr || false;

    this.startPort = opts.startPort || 31000;

    this.nodeOptions = {}    
    if( isObject( this.numNodes ) ){
        var len = 0
        for( var i in this.numNodes ){
            var options = this.nodeOptions[ "n" + len ] = this.numNodes[i]
            if( i.startsWith( "a" ) ) options.arbiter = true
            len++
        }
        this.numNodes = len
    }
    else if( Array.isArray( this.numNodes ) ){
        for( var i = 0; i < this.numNodes.length; i++ )
            this.nodeOptions[ "n" + i ] = this.numNodes[i]
        this.numNodes = this.numNodes.length
    }
    
    if(this.bridged) {
        this.bridgePorts = [];

        var allPorts = allocatePorts( this.numNodes * 2 , this.startPort );
        for(var i=0; i < this.numNodes; i++) {
            this.ports[i] = allPorts[i*2];
            this.bridgePorts[i] = allPorts[i*2 + 1];
        }

        this.initBridges();
    }
    else {
        this.ports = allocatePorts( this.numNodes , this.startPort );
    }

    this.nodes = []
    this.initLiveNodes()
    
    Object.extend( this, ReplSetTest.Health )
    Object.extend( this, ReplSetTest.State )
    
}

ReplSetTest.prototype.initBridges = function() {
    for(var i=0; i<this.ports.length; i++) {
        startMongoProgram( "mongobridge", "--port", this.bridgePorts[i], "--dest", this.host + ":" + this.ports[i] );
    }
}

// List of nodes as host:port strings.
ReplSetTest.prototype.nodeList = function() {
    var list = [];
    for(var i=0; i<this.ports.length; i++) {
      list.push( this.host + ":" + this.ports[i]);
    }

    return list;
}

// Here we store a reference to all reachable nodes.
ReplSetTest.prototype.initLiveNodes = function() {
    this.liveNodes = { master: null, slaves: [] }
}

ReplSetTest.prototype.getNodeId = function(node) {
    
    if( node.toFixed ) return parseInt( node )
    
    for( var i = 0; i < this.nodes.length; i++ ){
        if( this.nodes[i] == node ) return i
    }
    
    if( node instanceof ObjectId ){
        for( var i = 0; i < this.nodes.length; i++ ){
            if( this.nodes[i].runId == node ) return i
        }
    }
    
    if( node.nodeId ) return parseInt( node.nodeId )
    
    return undefined
    
}

ReplSetTest.prototype.getPort = function( n ){
    
    n = this.getNodeId( n )
    
    print( "ReplSetTest n: " + n + " ports: " + tojson( this.ports ) + "\t" + this.ports[n] + " " + typeof(n) );
    return this.ports[ n ];
}

ReplSetTest.prototype.getPath = function( n ){
    
    if( n.host )
        n = this.getNodeId( n )

    var p = "/data/db/" + this.name + "-"+n;
    if ( ! this._alldbpaths )
        this._alldbpaths = [ p ];
    else
        this._alldbpaths.push( p );
    return p;
}

ReplSetTest.prototype.getReplSetConfig = function() {
    var cfg = {};

    cfg['_id']  = this.name;
    cfg.members = [];

    for(i=0; i<this.ports.length; i++) {
        member = {};
        member['_id']  = i;

        if(this.bridged)
          var port = this.bridgePorts[i];
        else
          var port = this.ports[i];

        member['host'] = this.host + ":" + port;
        if( this.nodeOptions[ "n" + i ] && this.nodeOptions[ "n" + i ].arbiter )
            member['arbiterOnly'] = true
            
        cfg.members.push(member);
    }

    return cfg;
}

ReplSetTest.prototype.getURL = function(){
    var hosts = [];
    
    for(i=0; i<this.ports.length; i++) {

        // Don't include this node in the replica set list
        if(this.bridged && this.ports[i] == this.ports[n]) {
            continue;
        }
        
        var port;
        // Connect on the right port
        if(this.bridged) {
            port = this.bridgePorts[i];
        }
        else {
            port = this.ports[i];
        }
        
        var str = this.host + ":" + port;
        hosts.push(str);
    }
    
    return this.name + "/" + hosts.join(",");
}

ReplSetTest.prototype.getOptions = function( n , extra , putBinaryFirst ){

    if ( ! extra )
        extra = {};

    if ( ! extra.oplogSize )
        extra.oplogSize = this.oplogSize;

    var a = []


    if ( putBinaryFirst )
        a.push( "mongod" );

    if ( extra.noReplSet ) {
        delete extra.noReplSet;
    }
    else {
        a.push( "--replSet" );
        
        if( this.useSeedList ) {
            a.push( this.getURL() );
        }
        else {
            a.push( this.name );
        }
    }
    
    a.push( "--noprealloc", "--smallfiles" );

    a.push( "--rest" );

    a.push( "--port" );
    a.push( this.getPort( n ) );

    a.push( "--dbpath" );
    a.push( this.getPath( ( n.host ? this.getNodeId( n ) : n ) ) );
    
    if( this.keyFile ){
        a.push( "--keyFile" )
        a.push( keyFile )
    }        
    
    if( jsTestOptions().noJournal ) a.push( "--nojournal" )
    if( jsTestOptions().noJournalPrealloc ) a.push( "--nopreallocj" )
    if( jsTestOptions().keyFile && !this.keyFile) {
        a.push( "--keyFile" )
        a.push( jsTestOptions().keyFile )
    }
    
    for ( var k in extra ){
        var v = extra[k];
        if( k in MongoRunner.logicalOptions ) continue
        a.push( "--" + k );
        if ( v != null ){
            if( v.replace ){
                v = v.replace(/\$node/g, "" + ( n.host ? this.getNodeId( n ) : n ) )
                v = v.replace(/\$set/g, this.name )
                v = v.replace(/\$path/g, this.getPath( n ) )
            }
            a.push( v );
        }
    }

    return a;
}

ReplSetTest.prototype.startSet = function( options ) {
    
    var nodes = [];
    print( "ReplSetTest Starting Set" );

    for( n = 0 ; n < this.ports.length; n++ ) {
        node = this.start(n, options)
        nodes.push(node);
    }

    this.nodes = nodes;
    return this.nodes;
}

ReplSetTest.prototype.callIsMaster = function() {
  
  var master = null;
  this.initLiveNodes();
    
  for(var i=0; i<this.nodes.length; i++) {

    try {
      var n = this.nodes[i].getDB('admin').runCommand({ismaster:1});
      
      if(n['ismaster'] == true) {
        master = this.nodes[i]
        this.liveNodes.master = master
      }
      else {
        this.nodes[i].setSlaveOk();
        this.liveNodes.slaves.push(this.nodes[i]);
      }

    }
    catch(err) {
      print("ReplSetTest Could not call ismaster on node " + i);
    }
  }

  return master || false;
}

ReplSetTest.awaitRSClientHosts = function( conn, host, hostOk, rs ) {
    
    if( host.length ){
        for( var i = 0; i < host.length; i++ ) this.awaitOk( conn, host[i] )
        return
    }
    
    if( hostOk == undefined ) hostOk = { ok : true }
    if( host.host ) host = host.host
    if( rs && rs.getMaster ) rs = rs.name
    
    print( "Awaiting " + host + " to be " + tojson( hostOk ) + " for " + conn + " (rs: " + rs + ")" )
    
    var tests = 0
    assert.soon( function() {
        var rsClientHosts = conn.getDB( "admin" ).runCommand( "connPoolStats" )[ "replicaSets" ]
        if( tests++ % 10 == 0 ) 
            printjson( rsClientHosts )
        
        for ( rsName in rsClientHosts ){
            if( rs && rs != rsName ) continue
            for ( var i = 0; i < rsClientHosts[rsName].hosts.length; i++ ){
                var clientHost = rsClientHosts[rsName].hosts[ i ];
                if( clientHost.addr != host ) continue
                
                // Check that *all* host properties are set correctly
                var propOk = true
                for( var prop in hostOk ){
                    if( clientHost[prop] != hostOk[prop] ){ 
                        propOk = false
                        break
                    }
                }
                
                if( propOk ) return true;

            }
        }
        return false;
    }, "timed out waiting for replica set client to recognize hosts",
       3 * 20 * 1000 /* ReplicaSetMonitorWatcher updates every 20s */ )
    
}

ReplSetTest.prototype.awaitSecondaryNodes = function( timeout ) {
  var master = this.getMaster();
  var slaves = this.liveNodes.slaves;
  var len = slaves.length;

  jsTest.attempt({context: this, timeout: 60000, desc: "Awaiting secondaries"}, function() {
     var ready = true;
     for(var i=0; i<len; i++) {
       ready = ready && slaves[i].getDB("admin").runCommand({ismaster: 1})['secondary'];
     }

     return ready;
  });
}

ReplSetTest.prototype.getMaster = function( timeout ) {
  var tries = 0;
  var sleepTime = 500;
  var t = timeout || 000;
  var master = null;

  master = jsTest.attempt({context: this, timeout: 60000, desc: "Finding master"}, this.callIsMaster);
  return master;
}

ReplSetTest.prototype.getPrimary = ReplSetTest.prototype.getMaster

ReplSetTest.prototype.getSecondaries = function( timeout ){
    var master = this.getMaster( timeout )
    var secs = []
    for( var i = 0; i < this.nodes.length; i++ ){
        if( this.nodes[i] != master ){
            secs.push( this.nodes[i] )
        }
    }
    return secs
}

ReplSetTest.prototype.getSecondary = function( timeout ){
    return this.getSecondaries( timeout )[0];
}

ReplSetTest.prototype.status = function( timeout ){
    var master = this.callIsMaster()
    if( ! master ) master = this.liveNodes.slaves[0]
    return master.getDB("admin").runCommand({replSetGetStatus: 1})
}

// Add a node to the test set
ReplSetTest.prototype.add = function( config ) {
  if(this.ports.length == 0) {
    var nextPort = allocatePorts( 1, this.startPort )[0];
  }
  else {
    var nextPort = this.ports[this.ports.length-1] + 1;
  }
  print("ReplSetTest Next port: " + nextPort);
  this.ports.push(nextPort);
  printjson(this.ports);

  var nextId = this.nodes.length;
  printjson(this.nodes);
  print("ReplSetTest nextId:" + nextId);
  var newNode = this.start( nextId );
  
  return newNode;
}

ReplSetTest.prototype.remove = function( nodeId ) {
    nodeId = this.getNodeId( nodeId )
    this.nodes.splice( nodeId, 1 );
    this.ports.splice( nodeId, 1 );
}

ReplSetTest.prototype.initiate = function( cfg , initCmd , timeout ) {
    var master  = this.nodes[0].getDB("admin");
    var config  = cfg || this.getReplSetConfig();
    var cmd     = {};
    var cmdKey  = initCmd || 'replSetInitiate';
    var timeout = timeout || 30000;
    cmd[cmdKey] = config;
    printjson(cmd);

    jsTest.attempt({context:this, timeout: timeout, desc: "Initiate replica set"}, function() {
        var result = master.runCommand(cmd);
        printjson(result);
        return result['ok'] == 1;
    });

    // Setup authentication if running test with authentication
    if (jsTestOptions().keyFile && !this.keyFile) {
        if (!this.shardSvr) {
            master = this.getMaster();
            jsTest.addAuth(master);
        }
        jsTest.authenticateNodes(this.nodes);
    }
}

ReplSetTest.prototype.reInitiate = function() {
    var master  = this.nodes[0];
    var c = master.getDB("local")['system.replset'].findOne();
    var config  = this.getReplSetConfig();
    config.version = c.version + 1;
    this.initiate( config , 'replSetReconfig' );
}

ReplSetTest.prototype.getLastOpTimeWritten = function() {
    this.getMaster();
    jsTest.attempt({context : this, desc : "awaiting oplog query"},
                 function() {
                     try {
                         this.latest = this.liveNodes.master.getDB("local")['oplog.rs'].find({}).sort({'$natural': -1}).limit(1).next()['ts'];
                     }
                     catch(e) {
                         print("ReplSetTest caught exception " + e);
                         return false;
                     }
                     return true;
                 });
};

ReplSetTest.prototype.awaitReplication = function(timeout) {
    timeout = timeout || 30000;

    this.getLastOpTimeWritten();

    print("ReplSetTest " + this.latest);

    jsTest.attempt({context: this, timeout: timeout, desc: "awaiting replication"},
                 function() {
                     try {
                         var synced = true;
                         for(var i=0; i<this.liveNodes.slaves.length; i++) {
                             var slave = this.liveNodes.slaves[i];

                             // Continue if we're connected to an arbiter
                             if(res = slave.getDB("admin").runCommand({replSetGetStatus: 1})) {
                                 if(res.myState == 7) {
                                     continue;
                                 }
                             }

                             slave.getDB("admin").getMongo().setSlaveOk();
                             var log = slave.getDB("local")['oplog.rs'];
                             if(log.find({}).sort({'$natural': -1}).limit(1).hasNext()) {
                                 var entry = log.find({}).sort({'$natural': -1}).limit(1).next();
                                 printjson( entry );
                                 var ts = entry['ts'];
                                 print("ReplSetTest await TS for " + slave + " is " + ts.t+":"+ts.i + " and latest is " + this.latest.t+":"+this.latest.i);

                                 if (this.latest.t < ts.t || (this.latest.t == ts.t && this.latest.i < ts.i)) {
                                     this.latest = this.liveNodes.master.getDB("local")['oplog.rs'].find({}).sort({'$natural': -1}).limit(1).next()['ts'];
                                 }

                                 print("ReplSetTest await oplog size for " + slave + " is " + log.count());
                                 synced = (synced && friendlyEqual(this.latest,ts))
                             }
                             else {
                                 synced = false;
                             }
                         }

                         if(synced) {
                             print("ReplSetTest await synced=" + synced);
                         }
                         return synced;
                     }
                     catch (e) {
                         print("ReplSetTest.awaitReplication: caught exception "+e);

                         // we might have a new master now
                         this.getLastOpTimeWritten();

                         return false;
                     }
                 });
}

ReplSetTest.prototype.getHashes = function( db ){
    this.getMaster();
    var res = {};
    res.master = this.liveNodes.master.getDB( db ).runCommand( "dbhash" )
    res.slaves = this.liveNodes.slaves.map( function(z){ return z.getDB( db ).runCommand( "dbhash" ); } )
    return res;
}

/**
 * Starts up a server.  Options are saved by default for subsequent starts.
 * 
 * 
 * Options { remember : true } re-applies the saved options from a prior start.
 * Options { noRemember : true } ignores the current properties.
 * Options { appendOptions : true } appends the current options to those remembered.
 * Options { startClean : true } clears the data directory before starting.
 *
 * @param @param {int|conn|[int|conn]} n array or single server number (0, 1, 2, ...) or conn
 * @param {object} [options]
 * @param {boolean} [restart] If false, the data directory will be cleared 
 * before the server starts.  Defaults to false.
 * 
 */
ReplSetTest.prototype.start = function( n , options , restart , wait ){
    
    if( n.length ){
        
        var nodes = n
        var started = []
        
        for( var i = 0; i < nodes.length; i++ ){
            if( this.start( nodes[i], Object.merge({}, options), restart, wait ) ){
                started.push( nodes[i] )
            }
        }
        
        return started
        
    }
    
    print( "ReplSetTest n is : " + n )
    
    defaults = { useHostName : this.useHostName,
                 oplogSize : this.oplogSize, 
                 keyFile : this.keyFile, 
                 port : this.getPort( n ),
                 noprealloc : "",
                 smallfiles : "",
                 rest : "",
                 replSet : this.useSeedList ? this.getURL() : this.name,
                 dbpath : "$set-$node" }
    
    // TODO : should we do something special if we don't currently know about this node?
    n = this.getNodeId( n )
    
    options = Object.merge( defaults, options )
    options = Object.merge( options, this.nodeOptions[ "n" + n ] )
    
    options.restart = restart
            
    var pathOpts = { node : n, set : this.name }
    options.pathOpts = Object.merge( options.pathOpts || {}, pathOpts )
    
    if( tojson(options) != tojson({}) )
        printjson(options)

    // make sure to call getPath, otherwise folders wont be cleaned
    this.getPath(n);

    print("ReplSetTest " + (restart ? "(Re)" : "") + "Starting....");
    
    var rval = this.nodes[n] = MongoRunner.runMongod( options )
    
    if( ! rval ) return rval
    
    // Add replica set specific attributes
    this.nodes[n].nodeId = n
            
    printjson( this.nodes )
        
    wait = wait || false
    if( ! wait.toFixed ){
        if( wait ) wait = 0
        else wait = -1
    }
    
    if( wait < 0 ) return rval
    
    // Wait for startup
    this.waitForHealth( rval, this.UP, wait )
    
    return rval
    
}


/**
 * Restarts a db without clearing the data directory by default.  If the server is not
 * stopped first, this function will not work.  
 * 
 * Option { startClean : true } forces clearing the data directory.
 * 
 * @param {int|conn|[int|conn]} n array or single server number (0, 1, 2, ...) or conn
 */
ReplSetTest.prototype.restart = function( n , options, signal, wait ){
    // Can specify wait as third parameter, if using default signal
    if( signal == true || signal == false ){
        wait = signal
        signal = undefined
    }
    
    this.stop( n, signal, wait && wait.toFixed ? wait : true )
    started = this.start( n , options , true, wait );

    if (jsTestOptions().keyFile && !this.keyFile) {
        if (started.length) {
             // if n was an array of conns, start will return an array of connections
            for (var i = 0; i < started.length; i++) {
                jsTest.authenticate(started[i]);
            }
        } else {
            jsTest.authenticate(started);
        }
    }
    return started;
}

ReplSetTest.prototype.stopMaster = function( signal , wait ) {
    var master = this.getMaster();
    var master_id = this.getNodeId( master );
    return this.stop( master_id , signal , wait );
}

// Stops a particular node or nodes, specified by conn or id
ReplSetTest.prototype.stop = function( n , signal, wait /* wait for stop */ ){
        
    // Flatten array of nodes to stop
    if( n.length ){
        nodes = n
        
        var stopped = []
        for( var i = 0; i < nodes.length; i++ ){
            if( this.stop( nodes[i], signal, wait ) )
                stopped.push( nodes[i] )
        }
        
        return stopped
    }
    
    // Can specify wait as second parameter, if using default signal
    if( signal == true || signal == false ){
        wait = signal
        signal = undefined
    }
        
    wait = wait || false
    if( ! wait.toFixed ){
        if( wait ) wait = 0
        else wait = -1
    }
    
    var port = this.getPort( n );
    print('ReplSetTest stop *** Shutting down mongod in port ' + port + ' ***');
    var ret = MongoRunner.stopMongod( port , signal );
    
    if( ! ret || wait < 0 ) return ret
    
    // Wait for shutdown
    this.waitForHealth( n, this.DOWN, wait )
    
    return true
}


ReplSetTest.prototype.stopSet = function( signal , forRestart ) {
    for(i=0; i < this.ports.length; i++) {
        this.stop( i, signal );
    }
    if ( ! forRestart && this._alldbpaths ){
        print("ReplSetTest stopSet deleting all dbpaths");
        for( i=0; i<this._alldbpaths.length; i++ ){
            resetDbpath( this._alldbpaths[i] );
        }
    }

    print('ReplSetTest stopSet *** Shut down repl set - test worked ****' )
};


/**
 * Waits until there is a master node
 */
ReplSetTest.prototype.waitForMaster = function( timeout ){
    
    var master = undefined
    
    jsTest.attempt({context: this, timeout: timeout, desc: "waiting for master"}, function() {
        return ( master = this.getMaster() )
    });
    
    return master
}


/**
 * Wait for a health indicator to go to a particular state or states.
 * 
 * @param node is a single node or list of nodes, by id or conn
 * @param state is a single state or list of states
 * 
 */
ReplSetTest.prototype.waitForHealth = function( node, state, timeout ){
    this.waitForIndicator( node, state, "health", timeout )    
}

/**
 * Wait for a state indicator to go to a particular state or states.
 * 
 * @param node is a single node or list of nodes, by id or conn
 * @param state is a single state or list of states
 * 
 */
ReplSetTest.prototype.waitForState = function( node, state, timeout ){
    this.waitForIndicator( node, state, "state", timeout )
}

/**
 * Wait for a rs indicator to go to a particular state or states.
 * 
 * @param node is a single node or list of nodes, by id or conn
 * @param states is a single state or list of states
 * @param ind is the indicator specified
 * 
 */
ReplSetTest.prototype.waitForIndicator = function( node, states, ind, timeout ){
    
    if( node.length ){
        
        var nodes = node        
        for( var i = 0; i < nodes.length; i++ ){
            if( states.length )
                this.waitForIndicator( nodes[i], states[i], ind, timeout )
            else
                this.waitForIndicator( nodes[i], states, ind, timeout )
        }
        
        return;
    }    
    
    timeout = timeout || 30000;
    
    if( ! node.getDB ){
        node = this.nodes[node]
    }
    
    if( ! states.length ) states = [ states ]
    
    print( "ReplSetTest waitForIndicator " + ind + " on " + node )
    printjson( states )
    print( "ReplSetTest waitForIndicator from node " + node )
    
    var lastTime = null
    var currTime = new Date().getTime()
    var status = undefined
        
    jsTest.attempt({context: this, timeout: timeout, desc: "waiting for state indicator " + ind + " for " + timeout + "ms" }, function() {
        
        status = this.status()
        
        var printStatus = false
        if( lastTime == null || ( currTime = new Date().getTime() ) - (1000 * 5) > lastTime ){
            if( lastTime == null ) print( "ReplSetTest waitForIndicator Initial status ( timeout : " + timeout + " ) :" )
            printjson( status )
            lastTime = new Date().getTime()
            printStatus = true
        }

        if (typeof status.members == 'undefined') {
            return false;
        }

        for( var i = 0; i < status.members.length; i++ ){
            if( printStatus ) print( "Status for : " + status.members[i].name + ", checking " + node.host + "/" + node.name )
            if( status.members[i].name == node.host || status.members[i].name == node.name ){
                for( var j = 0; j < states.length; j++ ){
                    if( printStatus ) print( "Status " + " : " + status.members[i][ind] + "  target state : " + states[j] )
                    if( status.members[i][ind] == states[j] ) return true;
                }
            }
        }
        
        return false
        
    });
    
    print( "ReplSetTest waitForIndicator final status:" )
    printjson( status )
    
}

ReplSetTest.Health = {}
ReplSetTest.Health.UP = 1
ReplSetTest.Health.DOWN = 0

ReplSetTest.State = {}
ReplSetTest.State.PRIMARY = 1
ReplSetTest.State.SECONDARY = 2
ReplSetTest.State.RECOVERING = 3

/** 
 * Overflows a replica set secondary or secondaries, specified by id or conn.
 */
ReplSetTest.prototype.overflow = function( secondaries ){
    
    // Create a new collection to overflow, allow secondaries to replicate
    var master = this.getMaster()
    var overflowColl = master.getCollection( "_overflow.coll" )
    overflowColl.insert({ replicated : "value" })
    this.awaitReplication()
    
    this.stop( secondaries, undefined, 5 * 60 * 1000 )
        
    var count = master.getDB("local").oplog.rs.count();
    var prevCount = -1;
    
    // Keep inserting till we hit our capped coll limits
    while (count != prevCount) {
      
      print("ReplSetTest overflow inserting 10000");
      
      for (var i = 0; i < 10000; i++) {
          overflowColl.insert({ overflow : "value" });
      }
      prevCount = count;
      this.awaitReplication();
      
      count = master.getDB("local").oplog.rs.count();
      
      print( "ReplSetTest overflow count : " + count + " prev : " + prevCount );
      
    }
    
    // Restart all our secondaries and wait for recovery state
    this.start( secondaries, { remember : true }, true, true )
    this.waitForState( secondaries, this.RECOVERING, 5 * 60 * 1000 )
    
}




/**
 * Bridging allows you to test network partitioning.  For example, you can set
 * up a replica set, run bridge(), then kill the connection between any two
 * nodes x and y with partition(x, y).
 *
 * Once you have called bridging, you cannot reconfigure the replica set.
 */
ReplSetTest.prototype.bridge = function() {
    if (this.bridges) {
        print("ReplSetTest bridge bridges have already been created!");
        return;
    }
    
    var n = this.nodes.length;

    // create bridges
    this.bridges = [];
    for (var i=0; i<n; i++) {
        var nodeBridges = [];
        for (var j=0; j<n; j++) {
            if (i == j) {
                continue;
            }
            nodeBridges[j] = new ReplSetBridge(this, i, j);
        }
        this.bridges.push(nodeBridges);
    }
    print("ReplSetTest bridge bridges: " + this.bridges);
    
    // restart everyone independently
    this.stopSet(null, true);
    for (var i=0; i<n; i++) {
        this.restart(i, {noReplSet : true});
    }
    
    // create new configs
    for (var i=0; i<n; i++) {
        config = this.nodes[i].getDB("local").system.replset.findOne();
        
        if (!config) {
            print("ReplSetTest bridge couldn't find config for "+this.nodes[i]);
            printjson(this.nodes[i].getDB("local").system.namespaces.find().toArray());
            assert(false);
        }

        var updateMod = {"$set" : {}};
        for (var j = 0; j<config.members.length; j++) {
            if (config.members[j].host == this.host+":"+this.ports[i]) {
                continue;
            }

            updateMod['$set']["members."+j+".host"] = this.bridges[i][j].host;
        }
        print("ReplSetTest bridge for node " + i + ":");
        printjson(updateMod);
        this.nodes[i].getDB("local").system.replset.update({},updateMod);
    }

    this.stopSet(null, true);
    
    // start set
    for (var i=0; i<n; i++) {
        this.restart(i);
    }

    return this.getMaster();
};

/**
 * This kills the bridge between two nodes.  As parameters, specify the from and
 * to node numbers.
 *
 * For example, with a three-member replica set, we'd have nodes 0, 1, and 2,
 * with the following bridges: 0->1, 0->2, 1->0, 1->2, 2->0, 2->1.  We can kill
 * the connection between nodes 0 and 2 by calling replTest.partition(0,2) or
 * replTest.partition(2,0) (either way is identical). Then the replica set would
 * have the following bridges: 0->1, 1->0, 1->2, 2->1.
 */
ReplSetTest.prototype.partition = function(from, to) {
    this.bridges[from][to].stop();
    this.bridges[to][from].stop();
};

/**
 * This reverses a partition created by partition() above.
 */
ReplSetTest.prototype.unPartition = function(from, to) {
    this.bridges[from][to].start();
    this.bridges[to][from].start();
};

ReplSetBridge = function(rst, from, to) {
    var n = rst.nodes.length;

    var startPort = rst.startPort+n;
    this.port = (startPort+(from*n+to));
    this.host = rst.host+":"+this.port;

    this.dest = rst.host+":"+rst.ports[to];
    this.start();
};

ReplSetBridge.prototype.start = function() {
    var args = ["mongobridge", "--port", this.port, "--dest", this.dest];
    print("ReplSetBridge starting: "+tojson(args));
    this.bridge = startMongoProgram.apply( null , args );
    print("ReplSetBridge started " + this.bridge);
};

ReplSetBridge.prototype.stop = function() {
    print("ReplSetBridge stopping: " + this.port);
    stopMongod(this.port);
};

ReplSetBridge.prototype.toString = function() {
    return this.host+" -> "+this.dest;
};
