// db.js

if ( typeof DB == "undefined" ){                     
    DB = function( mongo , name ){
        this._mongo = mongo;
        this._name = name;
    }
}

DB.prototype.getMongo = function(){
    assert( this._mongo , "why no mongo!" );
    return this._mongo;
}

DB.prototype.getName = function(){
    return this._name;
}

DB.prototype.getCollection = function( name ){
    return new DBCollection( this._mongo , this , name , this._name + "." + name );
}

DB.prototype.runCommand = function( obj ){
    if ( typeof( obj ) == "string" ){
        var n = {};
        n[obj] = 1;
        obj = n;
    }
    return this.getCollection( "$cmd" ).findOne( obj );
}

DB.prototype._dbCommand = DB.prototype.runCommand;

DB.prototype.addUser = function( username , pass ){
    var c = this.getCollection( "system.users" );
    
    var u = c.findOne( { user : username } ) || { user : username };
    u.pwd = hex_md5( "mongo" + pass );
    print( tojson( u ) );

    c.save( u );
}

DB.prototype.auth = function( username , pass ){
    var n = this.runCommand( { getnonce : 1 } );

    var a = this.runCommand( 
        { 
            authenticate : 1 , 
            user : username , 
            nonce : n.nonce , 
            key : hex_md5( n.nonce + username + hex_md5( "mongo" + pass ) )
        }
    );

    return a.ok;
}

/**
  Create a new collection in the database.  Normally, collection creation is automatic.  You would
   use this function if you wish to specify special options on creation.

   If the collection already exists, no action occurs.
   
   <p>Options:</p>
   <ul>
   	<li>
     size: desired initial extent size for the collection.  Must be <= 1000000000.
           for fixed size (capped) collections, this size is the total/max size of the 
           collection.
    </li>
    <li>
     capped: if true, this is a capped collection (where old data rolls out).
    </li>
    <li> max: maximum number of objects if capped (optional).</li>
    </ul>

   <p>Example: </p>
   
   <code>db.createCollection("movies", { size: 10 * 1024 * 1024, capped:true } );</code>
 
 * @param {String} name Name of new collection to create 
 * @param {Object} options Object with options for call.  Options are listed above.
 * @return SOMETHING_FIXME
*/
DB.prototype.createCollection = function(name, opt) {
    var options = opt || {};
    var cmd = { create: name, capped: options.capped, size: options.size, max: options.max };
    var res = this._dbCommand(cmd);
    return res;
}

/**
 *  Returns the current profiling level of this database
 *  @return SOMETHING_FIXME or null on error
 */
 DB.prototype.getProfilingLevel  = function() { 
    var res = this._dbCommand( { profile: -1 } );
    return res ? res.was : null;
}


/**
  Erase the entire database.  (!)
 
 * @return Object returned has member ok set to true if operation succeeds, false otherwise.
*/
DB.prototype.dropDatabase = function() { 	
    return this._dbCommand( { dropDatabase: 1 } );
}


DB.prototype.help = function() {
    print("DB methods:");
    print("\tdb.auth(username, password)");
    print("\tdb.getMongo() get the server connection object");
    print("\tdb.getName()");
    print("\tdb.getCollection(cname) same as db['cname'] or db.cname");
    print("\tdb.runCommand(cmdObj) run a database command.  if cmdObj is a string, turns it into { cmdObj : 1 }");
    print("\tdb.addUser(username, password)");
    print("\tdb.createCollection(name, { size : ..., capped : ..., max : ... } )");
    print("\tdb.getProfilingLevel()");
    print("\tdb.setProfilingLevel(level) 0=off 1=slow 2=all");
    print("\tdb.eval(func, args) run code server-side");
    print("\tdb.getLastError()");
    print("\tdb.getPrevError()");
    print("\tdb.resetError()");
    print("\tdb.getCollectionNames()");
    print("\tdb.group(ns, key[, keyf], cond, reduce, initial)");
}

/**
 * <p> Set profiling level for your db.  Profiling gathers stats on query performance. </p>
 * 
 * <p>Default is off, and resets to off on a database restart -- so if you want it on,
 *    turn it on periodically. </p>
 *  
 *  <p>Levels :</p>
 *   <ul>
 *    <li>0=off</li>
 *    <li>1=log very slow (>100ms) operations</li>
 *    <li>2=log all</li>
 *  @param {String} level Desired level of profiling
 *  @return SOMETHING_FIXME or null on error
 */
DB.prototype.setProfilingLevel = function(level) {
    
    if (level < 0 || level > 2) { 
        throw { dbSetProfilingException : "input level " + level + " is out of range [0..2]" };        
    }
    
    if (level) {
	// if already exists does nothing
		this.createCollection("system.profile", { capped: true, size: 128 * 1024 } );
    }
    return this._dbCommand( { profile: level } );
}


/**
 *  <p> Evaluate a js expression at the database server.</p>
 * 
 * <p>Useful if you need to touch a lot of data lightly; in such a scenario
 *  the network transfer of the data could be a bottleneck.  A good example
 *  is "select count(*)" -- can be done server side via this mechanism.
 * </p>
 *
 * <p>
 * If the eval fails, an exception is thrown of the form:
 * </p>
 * <code>{ dbEvalException: { retval: functionReturnValue, ok: num [, errno: num] [, errmsg: str] } }</code>
 * 
 * <p>Example: </p>
 * <code>print( "mycount: " + db.eval( function(){db.mycoll.find({},{_id:ObjId()}).length();} );</code>
 *
 * @param {Function} jsfunction Javascript function to run on server.  Note this it not a closure, but rather just "code".
 * @return result of your function, or null if error
 * 
 */
DB.prototype.eval = function(jsfunction) {
    var cmd = { $eval : jsfunction };
    if ( arguments.length > 1 ) {
	cmd.args = argumentsToArray( arguments ).slice(1);
    }
    
    var res = this._dbCommand( cmd );
    
    if (!res.ok)
    	throw tojson( res );
    
    return res.retval;
}

DB.prototype.dbEval = DB.prototype.eval;


/**
 * 
 *  <p>
 *   Similar to SQL group by.  For example: </p>
 *
 *  <code>select a,b,sum(c) csum from coll where active=1 group by a,b</code>
 *
 *  <p>
 *    corresponds to the following in 10gen:
 *  </p>
 * 
 *  <code>
     db.group(
       {
         ns: "coll",
         key: { a:true, b:true },
	 // keyf: ...,
	 cond: { active:1 },
	 reduce: function(obj,prev) { prev.csum += obj.c; } ,
	 initial: { csum: 0 }
	 });
	 </code>
 *
 * 
 * <p>
 *  An array of grouped items is returned.  The array must fit in RAM, thus this function is not
 * suitable when the return set is extremely large.
 * </p>
 * <p>
 * To order the grouped data, simply sort it client side upon return.
 * <p>
   Defaults
     cond may be null if you want to run against all rows in the collection
     keyf is a function which takes an object and returns the desired key.  set either key or keyf (not both).
 * </p>
*/
DB.prototype.group = function(parmsObj) {
	
    var groupFunction = function() {
	var parms = args[0];
    	var c = db[parms.ns].find(parms.cond||{});
    	var map = new Map();
        
    	while( c.hasNext() ) {
	    var obj = c.next();
            
	    var key = {};
	    if( parms.key ) {
	    	for( var i in parms.key )
		    key[i] = obj[i];
	    }
	    else {
	    	key = parms.$keyf(obj);
	    }
            
	    var aggObj = map[key];
	    if( aggObj == null ) {
		var newObj = Object.extend({}, key); // clone
	    	aggObj = map[key] = Object.extend(newObj, parms.initial)
	    }
	    parms.$reduce(obj, aggObj);
	}
        
	var ret = map.values();
   	return ret;
    }
    
    var parms = Object.extend({}, parmsObj);
    
    if( parms.reduce ) {
	parms.$reduce = parms.reduce; // must have $ to pass to db
	delete parms.reduce;
    }
    
    if( parms.keyf ) {
	parms.$keyf = parms.keyf;
	delete parms.keyf;
    }
    
    return this.eval(groupFunction, parms);
}

DB.prototype.resetError = function(){
    return this.runCommand( { reseterror : 1 } );
}

DB.prototype.forceError = function(){
    return this.runCommand( { forceerror : 1 } );
}

DB.prototype.getLastError = function(){
    return this.runCommand( { getlasterror : 1 } ).err;
}

/* Return the last error which has occurred, even if not the very last error.

   Returns: 
    { err : <error message>, nPrev : <how_many_ops_back_occurred>, ok : 1 }

   result.err will be null if no error has occurred.
 */
DB.prototype.getPrevError = function(){
    return this.runCommand( { getpreverror : 1 } );
}

DB.prototype.getCollectionNames = function(){
    var all = [];

    var nsLength = this._name.length + 1;
    
    this.getCollection( "system.namespaces" ).find().forEach(
        function(z){
            var name = z.name;
            
            if ( name.indexOf( "$" ) >= 0 )
                return;
            
            all.push( name.substring( nsLength ) );
        }
    );
    return all;
}

DB.prototype.tojson = function(){
    return this.toString();
}

DB.prototype.toString = function(){
    return this._name;
}

