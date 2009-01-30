// mongo.js

// NOTE 'Mongo' may be defined here or in MongoJS.cpp.  Add code to init, not to this constructor.
if ( typeof Mongo == "undefined" ){
    Mongo = function( host ){
        this.init( host );
    }
}

Mongo.prototype.find = function( ns , query , fields , limit , skip ){ throw "find not implemented"; }
Mongo.prototype.insert = function( ns , obj ){ throw "insert not implemented"; }
Mongo.prototype.remove = function( ns , pattern ){ throw "remove not implemented;" }
Mongo.prototype.update = function( ns , query , obj ){ throw "update not implemented;" }

mongoInject( Mongo.prototype );

Mongo.prototype.setSlaveOk = function() {
    this.slaveOk = true;
}

Mongo.prototype.getDB = function( name ){
    return new DB( this , name );
}

Mongo.prototype.getDBs = function(){
    var res = this.getDB( "admin" ).runCommand( { "listDatabases" : 1 } );
    assert( res.ok == 1 , "listDatabases failed" );
    return res;
}

Mongo.prototype.getDBNames = function(){
    return this.getDBs().databases.map( 
        function(z){
            return z.name;
        }
    );
}

Mongo.prototype.toString = function(){
    return "mongo connection";
}

connect = function( url , user , pass ){
    print( "connecting to: " + url )

    if ( user && ! pass )
        throw "you specified a user and not a password.  either you need a password, or you're using the old connect api";

    var idx = url.indexOf( "/" );
    
    var db;
    
    if ( idx < 0 )
        db = new Mongo().getDB( url );
    else 
        db = new Mongo( url.substring( 0 , idx ) ).getDB( url.substring( idx + 1 ) );
    
    if ( user && pass ){
        if ( ! db.auth( user , pass ) ){
            throw "couldn't login";
        }
    }
    
    return db;
}

_portAndDbpath = function() {
    var port = "";
    var dbpath = "";
    for( i = 0; i < arguments.length; ++i )
        if ( arguments[ i ] == "--port" )
            port = arguments[ i + 1 ];
        else if ( arguments[ i ] == "--dbpath" )
            dbpath = arguments[ i + 1 ];
    
    if ( port == "" || dbpath == "" )
        throw "Invalid command line options";
    
    return { port: port, dbpath: dbpath };
}

// Start a mongod instance and return a 'Mongo' object connected to it.
// This function's arguments are passed as command line arguments to mongod.
// The specified 'dbpath' is cleared if it exists, created if not.
startMongod = function() {
    var dbpath = _portAndDbpath.apply( null, arguments ).dbpath;
    resetDbpath( dbpath );
    return startMongodNoReset.apply( null, arguments );
}

// Same as startDb, but uses existing files in dbpath
startMongodNoReset = function() {
    var port = _portAndDbpath.apply( null, arguments ).port;
    
    _startMongod.apply( null, arguments );
    
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

