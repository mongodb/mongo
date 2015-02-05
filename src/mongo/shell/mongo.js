// mongo.js

// NOTE 'Mongo' may be defined here or in MongoJS.cpp.  Add code to init, not to this constructor.
if ( typeof Mongo == "undefined" ){
    Mongo = function( host ){
        this.init( host );
    }
}

if ( ! Mongo.prototype ){
    throw Error("Mongo.prototype not defined");
}

if ( ! Mongo.prototype.find )
    Mongo.prototype.find = function( ns , query , fields , limit , skip , batchSize , options ){ throw Error("find not implemented"); }
if ( ! Mongo.prototype.insert )
    Mongo.prototype.insert = function( ns , obj ){ throw Error("insert not implemented"); }
if ( ! Mongo.prototype.remove )
    Mongo.prototype.remove = function( ns , pattern ){ throw Error("remove not implemented"); }
if ( ! Mongo.prototype.update )
    Mongo.prototype.update = function( ns , query , obj , upsert ){ throw Error("update not implemented"); }

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
    if ((jsTest.options().keyFile || jsTest.options().useX509) && 
         ((typeof this.authenticated == 'undefined') || !this.authenticated)) {
        jsTest.authenticate(this)
    }
    return new DB( this , name );
}

Mongo.prototype.getDBs = function(){
    var res = this.getDB( "admin" ).runCommand( { "listDatabases" : 1 } );
    if ( ! res.ok )
        throw Error( "listDatabases failed:" + tojson( res ) );
    return res;
}

Mongo.prototype.adminCommand = function( cmd ){
    return this.getDB( "admin" ).runCommand( cmd );
}

/**
 * Returns all log components and current verbosity values
 */
Mongo.prototype.getLogComponents = function() {
    var res = this.adminCommand({ getParameter:1, logComponentVerbosity:1 });
    if (!res.ok)
        throw Error( "getLogComponents failed:" + tojson(res));
    return res.logComponentVerbosity;
}

/**
 * Accepts optional second argument "component",
 * string of form "storage.journaling"
 */
Mongo.prototype.setLogLevel = function(logLevel, component) {
    componentNames = [];
    if (typeof component === "string") {
        componentNames = component.split(".");
    }
    else if (component !== undefined) {
        throw Error( "setLogLevel component must be a string:" + tojson(component));
    }
    var vDoc = { verbosity: logLevel };

    // nest vDoc
    for (var key,obj; componentNames.length > 0;) {
        obj = {};
        key = componentNames.pop();
        obj[ key ] = vDoc;
        vDoc = obj;
    }
    var res = this.adminCommand({ setParameter : 1, logComponentVerbosity : vDoc });
    if (!res.ok)
        throw Error( "setLogLevel failed:" + tojson(res));
    return res;
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
        throw Error("need . in ns");
    var db = ns.substring( 0 , idx );
    var c = ns.substring( idx + 1 );
    return this.getDB( db ).getCollection( c );
}

Mongo.prototype.toString = function(){
    return "connection to " + this.host;
}
Mongo.prototype.tojson = Mongo.prototype.toString;

/**
 * Sets the read preference.
 *
 * @param mode {string} read preference mode to use. Pass null to disable read
 *     preference.
 * @param tagSet {Array.<Object>} optional. The list of tags to use, order matters.
 *     Note that this object only keeps a shallow copy of this array.
 */
Mongo.prototype.setReadPref = function (mode, tagSet) {
    this._readPrefMode = mode;
    this._readPrefTagSet = tagSet;
};

Mongo.prototype.getReadPrefMode = function () {
    return this._readPrefMode;
};

Mongo.prototype.getReadPrefTagSet = function () {
    return this._readPrefTagSet;
};

connect = function(url, user, pass) {
    if (user && !pass)
        throw Error("you specified a user and not a password.  " +
                    "either you need a password, or you're using the old connect api");

    // Validate connection string "url" as "hostName:portNumber/databaseName"
    //                                  or "hostName/databaseName"
    //                                  or "databaseName"
    // hostName may be an IPv6 address (with colons), in which case ":portNumber" is required
    //
    var urlType = typeof url;
    if (urlType == "undefined") {
        throw Error("Missing connection string");
    }
    if (urlType != "string") {
        throw Error("Incorrect type \"" + urlType +
                    "\" for connection string \"" + tojson(url) + "\"");
    }
    url = url.trim();
    if (0 == url.length) {
        throw Error("Empty connection string");
    }
    var colon = url.lastIndexOf(":");
    var slash = url.lastIndexOf("/");
    if (0 == colon || 0 == slash) {
        throw Error("Missing host name in connection string \"" + url + "\"");
    }
    if (colon == slash - 1 || colon == url.length - 1) {
        throw Error("Missing port number in connection string \"" + url + "\"");
    }
    if (colon != -1 && colon < slash) {
        var portNumber = url.substring(colon + 1, slash);
        if (portNumber.length > 5 || !/^\d*$/.test(portNumber) || parseInt(portNumber) > 65535) {
            throw Error("Invalid port number \"" + portNumber +
                        "\" in connection string \"" + url + "\"");
        }
    }
    if (slash == url.length - 1) {
        throw Error("Missing database name in connection string \"" + url + "\"");
    }

    chatty("connecting to: " + url)
    var db;
    if (slash == -1)
        db = new Mongo().getDB(url);
    else 
        db = new Mongo(url.substring(0, slash)).getDB(url.substring(slash + 1));

    if (user && pass) {
        if (!db.auth(user, pass)) {
            throw Error("couldn't login");
        }
    }
    return db;
}

/** deprecated, use writeMode below
 * 
 */
Mongo.prototype.useWriteCommands = function() {
	return (this.writeMode() != "legacy");
}

Mongo.prototype.forceWriteMode = function( mode ) {
    this._writeMode = mode;
}

Mongo.prototype.hasWriteCommands = function() {
    if ( !('_hasWriteCommands' in this) ) {
        var isMaster = this.getDB("admin").runCommand({ isMaster : 1 });
        this._hasWriteCommands = (isMaster.ok && 
                                  'minWireVersion' in isMaster &&
                                  isMaster.minWireVersion <= 2 && 
                                  2 <= isMaster.maxWireVersion );
    }
    
    return this._hasWriteCommands;
}

Mongo.prototype.hasExplainCommand = function() {
    if ( !('_hasExplainCommand' in this) ) {
        var isMaster = this.getDB("admin").runCommand({ isMaster : 1 });
        this._hasExplainCommand = (isMaster.ok &&
                                   'minWireVersion' in isMaster &&
                                   isMaster.minWireVersion <= 3 &&
                                   3 <= isMaster.maxWireVersion );
    }

    return this._hasExplainCommand;
}

/**
 * {String} Returns the current mode set. Will be commands/legacy/compatibility
 * 
 * Sends isMaster to determine if the connection is capable of using bulk write operations, and
 * caches the result.
 */

Mongo.prototype.writeMode = function() {

    if ( '_writeMode' in this ) {
        return this._writeMode;
    }

    // get default from shell params
    if ( _writeMode )
        this._writeMode = _writeMode();
    
    // can't use "commands" mode unless server version is good.
    if ( this.hasWriteCommands() ) {
        // good with whatever is already set
    }
    else if ( this._writeMode == "commands" ) {
        print("Cannot use commands write mode, degrading to compatibility mode");
        this._writeMode = "compatibility";
    }
    
    return this._writeMode;
};



//
// Write Concern can be set at the connection level, and is used for all write operations unless
// overridden at the collection level.
//

Mongo.prototype.setWriteConcern = function( wc ) {
    if ( wc instanceof WriteConcern ) {
        this._writeConcern = wc;
    }
    else {
        this._writeConcern = new WriteConcern( wc );
    }
};

Mongo.prototype.getWriteConcern = function() {
    return this._writeConcern;
};

Mongo.prototype.unsetWriteConcern = function() {
    delete this._writeConcern;
};
