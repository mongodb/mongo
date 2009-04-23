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
    return "mongo connection to " + this.host;
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
