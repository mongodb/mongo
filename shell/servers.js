

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
    else {
        for ( var i=0; i<args.length; i++ )
            fullArgs.push( args[i] )
    }

    return fullArgs;
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
            oplogSize: "2",
            nohttpinterface: ""
        };

    if ( extraOptions )
        Object.extend( options , extraOptions );
    
    var conn = f.apply(null, [ options ] );

    conn.name = (useHostname ? getHostName() : "localhost") + ":" + port;
    return conn;
}

// Start a mongod instance and return a 'Mongo' object connected to it.
// This function's arguments are passed as command line arguments to mongod.
// The specified 'dbpath' is cleared if it exists, created if not.
startMongodEmpty = function () {
    var args = createMongoArgs("mongod", arguments);

    var dbpath = _parsePath.apply(null, args);
    resetDbpath(dbpath);

    return startMongoProgram.apply(null, args);
}
startMongod = function () {
    print("WARNING DELETES DATA DIRECTORY THIS IS FOR TESTING RENAME YOUR INVOCATION");
    return startMongodEmpty.apply(null, arguments);
}
startMongodNoReset = function(){
    var args = createMongoArgs( "mongod" , arguments );
    return startMongoProgram.apply( null, args );
}

startMongos = function(){
    return startMongoProgram.apply( null, createMongoArgs( "mongos" , arguments ) );
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
    }, "unable to connect to mongo program on port " + port, 60000 );

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
    this._testName = testName;

    if ( ! otherParams )
        otherParams = {}
    this._connections = [];
    
    if ( otherParams.sync && numShards < 3 )
        throw "if you want sync, you need at least 3 servers";

    var localhost = otherParams.useHostname ? getHostName() : "localhost";

    this._alldbpaths = []


    if ( otherParams.rs ){
        localhost = getHostName();
        // start replica sets
        this._rs = []
        for ( var i=0; i<numShards; i++){
            var rs = new ReplSetTest( { name : testName + "-rs" + i , nodes : 3 , startPort : 31100 + ( i * 100 ) } );
            this._rs[i] = { test : rs , nodes : rs.startSet( { oplogSize:40 } ) , url : rs.getURL() };
            rs.initiate();
            
        }

        for ( var i=0; i<numShards; i++){
            var rs = this._rs[i].test;
            rs.getMaster().getDB( "admin" ).foo.save( { x : 1 } )
            rs.awaitReplication();
            this._connections.push( new Mongo( rs.getURL() ) );
        }
        
        this._configServers = []
        for ( var i=0; i<3; i++ ){
            var conn = startMongodTest( 30000 + i , testName + "-config" + i );
            this._alldbpaths.push( testName + "-config" + i )
            this._configServers.push( conn );
        }
        
        this._configDB = localhost + ":30000," + localhost + ":30001," + localhost + ":30002";
        this._configConnection = new Mongo( this._configDB );
        this._configConnection.getDB( "config" ).settings.insert( { _id : "chunksize" , value : otherParams.chunksize || 50 } );        
    }
    else {
        for ( var i=0; i<numShards; i++){
            var conn = startMongodTest( 30000 + i , testName + i, 0, {useHostname : otherParams.useHostname} );
            this._alldbpaths.push( testName +i )
            this._connections.push( conn );
        }
        
        if ( otherParams.sync ){
            this._configDB = localhost+":30000,"+localhost+":30001,"+localhost+":30002";
            this._configConnection = new Mongo( this._configDB );
            this._configConnection.getDB( "config" ).settings.insert( { _id : "chunksize" , value : otherParams.chunksize || 50 } );        
        }
        else {
            this._configDB = localhost + ":30000";
            this._connections[0].getDB( "config" ).settings.insert( { _id : "chunksize" , value : otherParams.chunksize || 50 } );
        }
    }
    
    this._mongos = [];
    var startMongosPort = 31000;
    for ( var i=0; i<(numMongos||1); i++ ){
        var myPort =  startMongosPort - i;
        print("config: "+this._configDB);
        var conn = startMongos( { port : startMongosPort - i , v : verboseLevel || 0 , configdb : this._configDB }  );
        conn.name = localhost + ":" + myPort;
        this._mongos.push( conn );
        if ( i == 0 )
            this.s = conn;
    }

    var admin = this.admin = this.s.getDB( "admin" );
    this.config = this.s.getDB( "config" );

    if ( ! otherParams.manualAddShard ){
        this._connections.forEach(
            function(z){
                var n = z.name;
                if ( ! n ){
                    n = z.host;
                    if ( ! n )
                        n = z;
                }
                print( "going to add shard: " + n )
                x = admin.runCommand( { addshard : n } );
                printjson( x )
            }
        );
    }
}

ShardingTest.prototype.getDB = function( name ){
    return this.s.getDB( name );
}

ShardingTest.prototype.getServerName = function( dbname ){
    var x = this.config.databases.findOne( { _id : dbname } );
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

    for ( var i=0; i<this._connections.length; i++ ){
        var c = this._connections[i];
        if ( name == c.name )
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
    if ( this._connections.length != 2 )
        throw "getOther only works with 2 servers";

    if ( one._mongo )
        one = one._mongo

    if ( this._connections[0] == one )
        return this._connections[1];
    return this._connections[0];
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
        stopMongoProgram( 31000 - i );
    }
    for ( var i=0; i<this._connections.length; i++){
        stopMongod( 30000 + i );
    }
    if ( this._rs ){
        for ( var i=0; i<this._rs.length; i++ ){
            this._rs[i].test.stopSet( 15 );
        }
    }
    if ( this._alldbpaths ){
        for( i=0; i<this._alldbpaths.length; i++ ){
            resetDbpath( "/data/db/" + this._alldbpaths[i] );
        }
    }

    print('*** ' + this._testName + " completed successfully ***");
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

            print( msg )
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
    print( this.getChunksString( ns ) );
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

    print( out );
}

printShardingStatus = function( configDB ){
    if (configDB === undefined)
        configDB = db.getSisterDB('config')
    
    var version = configDB.getCollection( "version" ).findOne();
    if ( version == null ){
        print( "not a shard db!" );
        return;
    }
    
    var raw = "";
    var output = function(s){
        raw += s + "\n";
    }
    output( "--- Sharding Status --- " );
    output( "  sharding version: " + tojson( configDB.getCollection( "version" ).findOne() ) );
    
    output( "  shards:" );
    configDB.shards.find().forEach( 
        function(z){
            output( "      " + tojson(z) );
        }
    );

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
                                output( "\t\t\t" + tojson( chunk.min ) + " -->> " + tojson( chunk.max ) + 
                                        " on : " + chunk.shard + " " + tojson( chunk.lastmod ) );
                            }
                        );
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
        print( "not a shard db!" );
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

ShardingTest.prototype.shardGo = function( collName , key , split , move , dbName ){
    split = split || key;
    move = move || split;
    dbName = dbName || "test";

    var c = dbName + "." + collName;

    s.adminCommand( { shardcollection : c , key : key } );
    s.adminCommand( { split : c , middle : split } );
    s.adminCommand( { movechunk : c , find : move , to : this.getOther( s.getServer( dbName ) ).name } );
    
}

MongodRunner = function( port, dbpath, peer, arbiter, extraArgs ) {
    this.port_ = port;
    this.dbpath_ = dbpath;
    this.peer_ = peer;
    this.arbiter_ = arbiter;
    this.extraArgs_ = extraArgs;
}

MongodRunner.prototype.start = function( reuseData ) {
    var args = [];
    if ( reuseData ) {
        args.push( "mongod" );
    }
    args.push( "--port" );
    args.push( this.port_ );
    args.push( "--dbpath" );
    args.push( this.dbpath_ );
    if ( this.peer_ && this.arbiter_ ) {
        args.push( "--pairwith" );
        args.push( this.peer_ );
        args.push( "--arbiter" );
        args.push( this.arbiter_ );
        args.push( "--oplogSize" );
        // small oplog by default so startup fast
        args.push( "1" );
    }
    args.push( "--nohttpinterface" );
    args.push( "--noprealloc" );
    args.push( "--smallfiles" );
    args.push( "--bind_ip" );
    args.push( "127.0.0.1" );
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


ReplPair = function( left, right, arbiter ) {
    this.left_ = left;
    this.leftC_ = null;
    this.right_ = right;
    this.rightC_ = null;
    this.arbiter_ = arbiter;
    this.arbiterC_ = null;
    this.master_ = null;
    this.slave_ = null;
}

ReplPair.prototype.start = function( reuseData ) {
    if ( this.arbiterC_ == null ) {
        this.arbiterC_ = this.arbiter_.start();
    }
    if ( this.leftC_ == null ) {
        this.leftC_ = this.left_.start( reuseData );
    }
    if ( this.rightC_ == null ) {
        this.rightC_ = this.right_.start( reuseData );
    }
}

ReplPair.prototype.isMaster = function( mongo, debug ) {
    var im = mongo.getDB( "admin" ).runCommand( { ismaster : 1 } );
    assert( im && im.ok, "command ismaster failed" );
    if ( debug ) {
        printjson( im );
    }
    return im.ismaster;
}

ReplPair.prototype.isInitialSyncComplete = function( mongo, debug ) {
    var isc = mongo.getDB( "admin" ).runCommand( { isinitialsynccomplete : 1 } );
    assert( isc && isc.ok, "command isinitialsynccomplete failed" );
    if ( debug ) {
        printjson( isc );
    }
    return isc.initialsynccomplete;
}

ReplPair.prototype.checkSteadyState = function( state, expectedMasterHost, twoMasterOk, leftValues, rightValues, debug ) {
    leftValues = leftValues || {};
    rightValues = rightValues || {};

    var lm = null;
    var lisc = null;
    if ( this.leftC_ != null ) {
        lm = this.isMaster( this.leftC_, debug );
        leftValues[ lm ] = true;
        lisc = this.isInitialSyncComplete( this.leftC_, debug );
    }
    var rm = null;
    var risc = null;
    if ( this.rightC_ != null ) {
        rm = this.isMaster( this.rightC_, debug );
        rightValues[ rm ] = true;
        risc = this.isInitialSyncComplete( this.rightC_, debug );
    }

    var stateSet = {}
    state.forEach( function( i ) { stateSet[ i ] = true; } );
    if ( !( 1 in stateSet ) || ( ( risc || risc == null ) && ( lisc || lisc == null ) ) ) {
        if ( rm == 1 && lm != 1 ) {
            assert( twoMasterOk || !( 1 in leftValues ) );
            this.master_ = this.rightC_;
            this.slave_ = this.leftC_;
        } else if ( lm == 1 && rm != 1 ) {
            assert( twoMasterOk || !( 1 in rightValues ) );
            this.master_ = this.leftC_;
            this.slave_ = this.rightC_;
        }
        if ( !twoMasterOk ) {
            assert( lm != 1 || rm != 1, "two masters" );
        }
        // check for expected state
        if ( state.sort().toString() == [ lm, rm ].sort().toString() ) {
            if ( expectedMasterHost != null ) {
                if( expectedMasterHost == this.master_.host ) {
                    return true;
                }
            } else {
                return true;
            }
        }
    }

    this.master_ = null;
    this.slave_ = null;
    return false;
}

ReplPair.prototype.waitForSteadyState = function( state, expectedMasterHost, twoMasterOk, debug ) {
    state = state || [ 1, 0 ];
    twoMasterOk = twoMasterOk || false;
    var rp = this;
    var leftValues = {};
    var rightValues = {};
    assert.soon( function() { return rp.checkSteadyState( state, expectedMasterHost, twoMasterOk, leftValues, rightValues, debug ); },
                 "rp (" + rp + ") failed to reach expected steady state (" + state + ")" , 60000 );
}

ReplPair.prototype.master = function() { return this.master_; }
ReplPair.prototype.slave = function() { return this.slave_; }
ReplPair.prototype.right = function() { return this.rightC_; }
ReplPair.prototype.left = function() { return this.leftC_; }
ReplPair.prototype.arbiter = function() { return this.arbiterC_; }

ReplPair.prototype.killNode = function( mongo, signal ) {
    signal = signal || 15;
    if ( this.leftC_ != null && this.leftC_.host == mongo.host ) {
        stopMongod( this.left_.port_ );
        this.leftC_ = null;
    }
    if ( this.rightC_ != null && this.rightC_.host == mongo.host ) {
        stopMongod( this.right_.port_ );
        this.rightC_ = null;
    }
    if ( this.arbiterC_ != null && this.arbiterC_.host == mongo.host ) {
        stopMongod( this.arbiter_.port_ );
        this.arbiterC_ = null;
    }
}

ReplPair.prototype._annotatedNode = function( mongo ) {
    var ret = "";
    if ( mongo != null ) {
        ret += " (connected)";
        if ( this.master_ != null && mongo.host == this.master_.host ) {
            ret += "(master)";
        }
        if ( this.slave_ != null && mongo.host == this.slave_.host ) {
            ret += "(slave)";
        }
    }
    return ret;
}

ReplPair.prototype.toString = function() {
    var ret = "";
    ret += "left: " + this.left_;
    ret += " " + this._annotatedNode( this.leftC_ );
    ret += " right: " + this.right_;
    ret += " " + this._annotatedNode( this.rightC_ );
    return ret;
}

ToolTest = function( name ){
    this.name = name;
    this.port = allocatePorts(1)[0];
    this.baseName = "jstests_tool_" + name;
    this.root = "/data/db/" + this.baseName;
    this.dbpath = this.root + "/";
    this.ext = this.root + "_external/";
    this.extFile = this.root + "_external/a";
    resetDbpath( this.dbpath );
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

    runMongoProgram.apply( null , a );
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
        extra.oplogSize = "1";
        
    var a = []
    if ( putBinaryFirst )
        a.push( "mongod" )
    a.push( "--nohttpinterface", "--noprealloc", "--bind_ip" , "127.0.0.1" , "--smallfiles" );

    a.push( "--port" );
    a.push( this.getPort( master ) );

    a.push( "--dbpath" );
    a.push( this.getPath( master ) );
    

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


function startParallelShell( jsCode ){
    var x = startMongoProgramNoConnect( "mongo" , "--eval" , jsCode , db ? db.getMongo().host : null );
    return function(){
        waitProgram( x );
    };
}

var testingReplication = false;

function skipIfTestingReplication(){
    if (testingReplication) {
	print( "skipping" );
	quit(0);
    }
}

ReplSetTest = function( opts ){
    this.name  = opts.name || "testReplSet";
    this.host  = opts.host || getHostName();
    this.numNodes = opts.nodes || 0;
    this.oplogSize = opts.oplogSize || 2;
    this.useSeedList = opts.useSeedList || false;
    this.bridged = opts.bridged || false;
    this.ports = [];

    this.startPort = opts.startPort || 31000;

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

    this.nodes = [];
    this.nodeIds = {};
    this.initLiveNodes();
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
ReplSetTest.prototype.initLiveNodes = function(){
    this.liveNodes = {master: null, slaves: []};
    this.nodeIds   = {};
}

ReplSetTest.prototype.getNodeId = function(node) {
    return this.nodeIds[node];
}

ReplSetTest.prototype.getPort = function( n ){
    return this.ports[ n ];
}

ReplSetTest.prototype.getPath = function( n ){
    var p = "/data/db/" + this.name + "-";
    p += n.toString();
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
        a.push( "mongod" )

    a.push( "--replSet" );

    if( this.useSeedList ) {
      a.push( this.getURL() );
    }
    else {
      a.push( this.name );
    }

    a.push( "--noprealloc", "--smallfiles" );

    a.push( "--rest" );

    a.push( "--port" );
    a.push( this.getPort( n ) );

    a.push( "--dbpath" );
    a.push( this.getPath( n ) );

    for ( var k in extra ){
        var v = extra[k];
        a.push( "--" + k );
        if ( v != null )
            a.push( v );
    }

    return a;
}

ReplSetTest.prototype.startSet = function(options) {
    var nodes = [];
    print( "Starting Set" );

    for(n=0; n<this.ports.length; n++) {
        node = this.start(n, options);
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
        master = this.nodes[i];
        this.liveNodes.master = master;
        this.nodeIds[master] = i;
      }
      else {
        this.nodes[i].setSlaveOk();
        this.liveNodes.slaves.push(this.nodes[i]);
        this.nodeIds[this.nodes[i]] = i;
      }

    }
    catch(err) {
      print("Could not call ismaster on node " + i);
    }
  }

  return master || false;
}

ReplSetTest.prototype.awaitSecondaryNodes = function( timeout ) {
  var master = this.getMaster();
  var slaves = this.liveNodes.slaves;
  var len = slaves.length;

  this.attempt({context: this, timeout: 60000, desc: "Awaiting secondaries"}, function() {
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

  master = this.attempt({context: this, timeout: 60000, desc: "Finding master"}, this.callIsMaster);
  return master;
}

// Add a node to the test set
ReplSetTest.prototype.add = function( config ) {
  if(this.ports.length == 0) {
    var nextPort = allocatePorts( 1, this.startPort )[0];
  }
  else {
    var nextPort = this.ports[this.ports.length-1] + 1;
  }
  print("Next port: " + nextPort);
  this.ports.push(nextPort);
  printjson(this.ports);

  var nextId = this.nodes.length;
  printjson(this.nodes);
  print(nextId);
  var newNode = this.start(nextId);
  this.nodes.push(newNode);

  return newNode;
}

ReplSetTest.prototype.remove = function( nodeId ) {
    this.nodes.splice( nodeId, 1 );
    this.ports.splice( nodeId, 1 );
}

// Pass this method a function to call repeatedly until
// that function returns true. Example:
//   attempt({timeout: 20000, desc: "get master"}, function() { // return false until success })
ReplSetTest.prototype.attempt = function( opts, func ) {
    var timeout = opts.timeout || 1000;
    var tries   = 0;
    var sleepTime = 500;
    var result = null;
    var context = opts.context || this;

    while((result = func.apply(context)) == false) {
        tries += 1;
        sleep(sleepTime);
        if( tries * sleepTime > timeout) {
            throw('[' + opts['desc'] + ']' + " timed out");
        }
    }

    return result;
}

ReplSetTest.prototype.initiate = function( cfg , initCmd , timeout ) {
    var master  = this.nodes[0].getDB("admin");
    var config  = cfg || this.getReplSetConfig();
    var cmd     = {};
    var cmdKey  = initCmd || 'replSetInitiate';
    var timeout = timeout || 30000;
    cmd[cmdKey] = config;
    printjson(cmd);

    this.attempt({timeout: timeout, desc: "Initiate replica set"}, function() {
        var result = master.runCommand(cmd);
        printjson(result);
        return result['ok'] == 1;
    });
}

ReplSetTest.prototype.reInitiate = function() {
    var master  = this.nodes[0];
    var c = master.getDB("local")['system.replset'].findOne();
    var config  = this.getReplSetConfig();
    config.version = c.version + 1;
    this.initiate( config , 'replSetReconfig' );
}

ReplSetTest.prototype.awaitReplication = function() {
   this.getMaster();

   latest = this.liveNodes.master.getDB("local")['oplog.rs'].find({}).sort({'$natural': -1}).limit(1).next()['ts']
   print(latest);

   this.attempt({context: this, timeout: 30000, desc: "awaiting replication"},
       function() {
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
               print("TS for " + slave + " is " + ts + " and latest is " + latest);
               print("Oplog size for " + slave + " is " + log.count());
               synced = (synced && friendlyEqual(latest,ts))
             }
             else {
               synced = false;
             }
           }

           if(synced) {
             print("Synced = " + synced);
           }
           return synced;
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
 * Starts up a server.
 *
 * @param {int} n server number (0, 1, 2, ...)
 * @param {object} [options]
 * @param {boolean} [restart] If false, the data directory will be cleared 
 * before the server starts.  Defaults to false.
 */
ReplSetTest.prototype.start = function( n , options , restart ){
    var lockFile = this.getPath( n ) + "/mongod.lock";
    removeFile( lockFile );
    var o = this.getOptions( n , options , restart );

    print("Starting....");
    print( o );
    if ( restart ) {
        this.nodes[n] = startMongoProgram.apply( null , o );
        printjson(this.nodes);
        return this.nodes[n];
    }
    else {
        return startMongod.apply( null , o );
    }
}

/**
 * Restarts a db without clearing the data directory.  If the server is not
 * stopped first, this function will not work.
 * 
 * @param {int} n server number (0, 1, 2, ...)
 */
ReplSetTest.prototype.restart = function( n , options ){
    return this.start( n , options , true );
}

ReplSetTest.prototype.stop = function( n , signal ){
    var port = this.getPort( n );
    print('*** Shutting down mongod in port ' + port + ' ***');
    return stopMongod( port , signal || 15 );
}

ReplSetTest.prototype.stopSet = function( signal , forRestart ) {
    for(i=0; i < this.ports.length; i++) {
        this.stop( i, signal );
    }
    if ( ! forRestart && this._alldbpaths ){
        for( i=0; i<this._alldbpaths.length; i++ ){
            resetDbpath( this._alldbpaths[i] );
        }
    }

    print('*** Shut down repl set - test worked ****' )
}
