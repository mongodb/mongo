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

ToolTest = function( name, extraOptions ){
    this.name = name;
    this.options = extraOptions;
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

    var options = {port : this.port,
                   dbpath : this.dbpath,
                   nohttpinterface : "",
                   noprealloc : "",
                   smallfiles : "",
                   bind_ip : "127.0.0.1"};

    Object.extend(options, this.options);

    this.m = startMongoProgram.apply(null, MongoRunner.arrOptions("mongod", options));
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


    if (restart) {
        return startMongoProgram.apply(null, o);
    } else {
        var conn = startMongod.apply(null, o);
        if (jsTestOptions().keyFile || jsTestOptions().auth) {
            if (master) {
                jsTest.addAuth(conn);
            }
            jsTest.authenticate(conn);
        }
        return conn;
    }
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

    if (typeof(db) == "object") {
        jsCode = "db = db.getSiblingDB('" + db.getName() + "');" + jsCode;
    }

    if (TestData) {
        jsCode = "TestData = " + tojson(TestData) + ";" + jsCode;
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
