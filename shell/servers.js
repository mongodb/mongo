

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
    } );
    
    return m;
}

ShardingTest = function( testName , numServers , verboseLevel , numMongos ){
    this._connections = [];
    this._serverNames = [];

    for ( var i=0; i<numServers; i++){
        var conn = startMongod( { port : 30000 + i , dbpath : "/data/db/" + testName + i } );
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
            admin.runCommand( { addserver : z } );
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

MongodRunner = function( port, dbpath, peer, arbiter ) {
    this.port_ = port;
    this.dbpath_ = dbpath;
    this.peer_ = peer;
    this.arbiter_ = arbiter;
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
        args.push( this.arbiter_ );
        args.push( "--oplogSize" );
        // small oplog by default so startup fast
        args.push( "1" );
    }
    args.push( "--nohttpinterface" );
    if ( reuseData ) {
        return startMongoProgram.apply( null, args );
    } else {
        return startMongod.apply( null, args );
    }
}

MongodRunner.prototype.port = function() { return this.port_; }

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

ReplPair.prototype.isMaster = function( mongo ) {
    var im = mongo.getDB( "admin" ).runCommand( { ismaster : 1 } );
    assert( im && im.ok, "command ismaster failed" );
    printjson( im );
    return im.ismaster;
}

ReplPair.prototype.isInitialSyncComplete = function( mongo ) {
    var isc = mongo.getDB( "admin" ).runCommand( { isinitialsynccomplete : 1 } );
    assert( isc && isc.ok, "command isinitialsynccomplete failed" );
    printjson( isc );
    return isc.initialsynccomplete;
}

ReplPair.prototype.checkSteadyState = function( leftValues, rightValues ) {
    leftValues = leftValues || {};
    rightValues = rightValues || {};
    
    var lm = null;
    var lisc = null;
    if ( this.leftC_ != null ) {
        lm = this.isMaster( this.leftC_ );
        leftValues[ lm ] = true;
        lisc = this.isInitialSyncComplete( this.leftC_ );
    }
    var rm = null;
    var risc = null;
    if ( this.rightC_ != null ) {
        rm = this.isMaster( this.rightC_ );
        rightValues[ rm ] = true;
        risc = this.isInitialSyncComplete( this.rightC_ );
    }
    
    if ( ( risc || risc == null ) && ( lisc || lisc == null ) ) {
        if ( rm == 1 && ( lm == null || lm == 0 ) ) {
            assert( !( 1 in leftValues ) );
            this.master_ = this.rightC_;
            this.slave_ = this.leftC_;
            return true;
        } else if ( lm == 1 && ( rm == null || rm == 0 ) ) {
            assert( !( 1 in rightValues ) );
            this.master_ = this.leftC_;
            this.slave_ = this.rightC_;        
            return true;
        }
    }
    
    this.master_ = null;
    this.slave_ = null;
    return false;
}

ReplPair.prototype.waitForSteadyState = function() {
    var rp = this;
    var leftValues = {};
    var rightValues = {};
    assert.soon( function() { return rp.checkSteadyState( leftValues, rightValues ); } );
}

ReplPair.prototype.master = function() { return this.master_; }
ReplPair.prototype.slave = function() { return this.slave_; }

ReplPair.prototype.killNode = function( mongo, signal ) {
    signal = signal || 15;
    if ( this.leftC_.host == mongo.host ) {
        stopMongod( this.left_.port_ );
        this.leftC_ = null;
    }
    if ( this.rightC_.host == mongo.host ) {
        stopMongod( this.right_.port_ );
        this.rightC_ = null;
    }    
}
