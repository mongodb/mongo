

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
    }, "unable to connect to mongo program on port " + port, 30000 );

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

ShardingTest = function( testName , numServers , verboseLevel , numMongos ){
    this._connections = [];
    this._serverNames = [];

    for ( var i=0; i<numServers; i++){
        var conn = startMongod( { port : 30000 + i , dbpath : "/data/db/" + testName + i , 
            noprealloc : "" , smallfiles : "" , oplogSize : "2" } );
        conn.name = "localhost:" + ( 30000 + i );

        this._connections.push( conn );
        this._serverNames.push( conn.name );
    }

    this._configDB = "localhost:30000";


    this._mongos = [];
    var startMongosPort = 39999;
    for ( var i=0; i<(numMongos||1); i++ ){
        var myPort =  startMongosPort - i;
        var conn = startMongos( { port : startMongosPort - i , v : verboseLevel || 0 , configdb : this._configDB }  );
        conn.name = "localhost:" + myPort;
        this._mongos.push( conn );
        if ( i == 0 )
            this.s = conn;
    }

    var admin = this.admin = this.s.getDB( "admin" );
    this.config = this.s.getDB( "config" );

    this._serverNames.forEach(
        function(z){
            admin.runCommand( { addshard : z , allowLocal : true } );
        }
    );
}

ShardingTest.prototype.getDB = function( name ){
    return this.s.getDB( name );
}

ShardingTest.prototype.getServerName = function( dbname ){
    return this.config.databases.findOne( { name : dbname } ).primary;
}

ShardingTest.prototype.getServer = function( dbname ){
    var name = this.getServerName( dbname );
    for ( var i=0; i<this._serverNames.length; i++ ){
        if ( name == this._serverNames[i] )
            return this._connections[i];
    }
    throw "can't find server for: " + dbname + " name:" + name;

}

ShardingTest.prototype.getOther = function( one ){
    if ( this._connections.length != 2 )
        throw "getOther only works with 2 servers";

    if ( this._connections[0] == one )
        return this._connections[1];
    return this._connections[0];
}

ShardingTest.prototype.stop = function(){
    for ( var i=0; i<this._mongos.length; i++ ){
        stopMongoProgram( 39999 - i );
    }
    for ( var i=0; i<this._connections.length; i++){
        stopMongod( 30000 + i );
    }
}

ShardingTest.prototype.adminCommand = function(cmd){
    var res = this.admin.runCommand( cmd );
    if ( res && res.ok == 1 )
        return true;

    throw "command " + tojson( cmd ) + " failed: " + tojson( res );
}

ShardingTest.prototype.getChunksString = function( ns ){
    var q = {}
    if ( ns )
        q.ns = ns;
    return Array.tojson( this.config.chunks.find( q ).toArray() , "\n" );
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
    
    print( out );
}

printShardingStatus = function( configDB ){
    
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
        function(z){
            output( "\t" + tojson(z,"",true) );
        
            output( "\t\tmy chunks" );
            
            configDB.chunks.find( { "ns" : new RegExp( "^" + z.name ) } ).sort( { ns : 1 } ).forEach( 
                function(z){
                    output( "\t\t\t" + z.ns + " " + tojson( z.min ) + " -->> " + tojson( z.max ) + 
                           " on : " + z.shard + " " + tojson( z.lastmod ) );
                }
            );
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
                "rp (" + rp + ") failed to reach expected steady state (" + state + ")" );
}

ReplPair.prototype.master = function() { return this.master_; }
ReplPair.prototype.slave = function() { return this.slave_; }
ReplPair.prototype.right = function() { return this.rightC_; }
ReplPair.prototype.left = function() { return this.leftC_; }

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


ReplTest = function( name ){
    this.name = name;
    this.ports = allocatePorts( 2 );
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


ReplTest.prototype.getOptions = function( master , extra , putBinaryFirst ){

    if ( ! extra )
        extra = {};

    if ( ! extra.oplogSize )
        extra.oplogSize = 10;
        
    var a = []
    if ( putBinaryFirst )
        a.push( "mongod" )
    a.push( "--nohttpinterface", "--noprealloc", "--bind_ip" , "127.0.0.1" , "--smallfiles" );

    a.push( "--port" );
    a.push( this.getPort( master ) );

    a.push( "--dbpath" );
    a.push( this.getPath( master ) );
    

    if ( master ){
        a.push( "--master" );
    }
    else {
        a.push( "--slave" );
        a.push( "--source" );
        a.push( "127.0.0.1:" + this.ports[0] );
    }
    
    for ( var k in extra ){
        var v = extra[k];
        a.push( "--" + k );
        if ( v != null )
            a.push( v );                    
    }

    return a;
}

ReplTest.prototype.start = function( master , options , restart ){
    var o = this.getOptions( master , options , restart );
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
    stopMongod( this.getPort( master ) , signal || 15 );
}
