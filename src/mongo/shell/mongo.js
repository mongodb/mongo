// mongo.js

// NOTE 'Mongo' may be defined here or in MongoJS.cpp.  Add code to init, not to this constructor.
if ( typeof Mongo == "undefined" ){
    Mongo = function( host ){
        this.init( host );
    }
}

if ( ! Mongo.prototype ){
    throw "Mongo.prototype not defined";
}

if ( ! Mongo.prototype.find )
    Mongo.prototype.find = function( ns , query , fields , limit , skip , batchSize , options ){ throw "find not implemented"; }
if ( ! Mongo.prototype.insert )
    Mongo.prototype.insert = function( ns , obj ){ throw "insert not implemented"; }
if ( ! Mongo.prototype.remove )
    Mongo.prototype.remove = function( ns , pattern ){ throw "remove not implemented;" }
if ( ! Mongo.prototype.update )
    Mongo.prototype.update = function( ns , query , obj , upsert ){ throw "update not implemented;" }

if ( typeof mongoInject == "function" ){
    mongoInject( Mongo.prototype );
}

Mongo.prototype.setSlaveOk = function( value ) {
    if( value == undefined ) value = true;
    this.slaveOk = value;
}

Mongo.prototype.getSlaveOk = function() {
    return this.slaveOk || false;
}

Mongo.prototype.getDB = function( name ){
    if (jsTest.options().keyFile && ((typeof this.authenticated == 'undefined') || !this.authenticated)) {
        jsTest.authenticate(this)
    }
    return new DB( this , name );
}

Mongo.prototype.getDBs = function(){
    var res = this.getDB( "admin" ).runCommand( { "listDatabases" : 1 } );
    if ( ! res.ok )
        throw "listDatabases failed:" + tojson( res );
    return res;
}

Mongo.prototype.adminCommand = function( cmd ){
    return this.getDB( "admin" ).runCommand( cmd );
}

Mongo.prototype.setLogLevel = function( logLevel ){
    return this.adminCommand({ setParameter : 1, logLevel : logLevel })
}

Mongo.prototype.getDBNames = function(){
    return this.getDBs().databases.map( 
        function(z){
            return z.name;
        }
    );
}

Mongo.prototype.getCollection = function(ns){
    var idx = ns.indexOf( "." );
    if ( idx < 0 ) 
        throw "need . in ns";
    var db = ns.substring( 0 , idx );
    var c = ns.substring( idx + 1 );
    return this.getDB( db ).getCollection( c );
}

Mongo.prototype.toString = function(){
    return "connection to " + this.host;
}
Mongo.prototype.tojson = Mongo.prototype.toString;

connect = function( url , user , pass ){
    chatty( "connecting to: " + url )

    if ( user && ! pass )
        throw "you specified a user and not a password.  either you need a password, or you're using the old connect api";

    var idx = url.lastIndexOf( "/" );
    
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
