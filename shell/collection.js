// @file collection.js - DBCollection support in the mongo shell
// db.colName is a DBCollection object
// or db["colName"]

if ( ( typeof  DBCollection ) == "undefined" ){
    DBCollection = function( mongo , db , shortName , fullName ){
        this._mongo = mongo;
        this._db = db;
        this._shortName = shortName;
        this._fullName = fullName;

        this.verify();
    }
}

DBCollection.prototype.verify = function(){
    assert( this._fullName , "no fullName" );
    assert( this._shortName , "no shortName" );
    assert( this._db , "no db" );

    assert.eq( this._fullName , this._db._name + "." + this._shortName , "name mismatch" );

    assert( this._mongo , "no mongo in DBCollection" );
}

DBCollection.prototype.getName = function(){
    return this._shortName;
}

DBCollection.prototype.help = function () {
    var shortName = this.getName();
    print("DBCollection help");
    print("\tdb." + shortName + ".find().help() - show DBCursor help");
    print("\tdb." + shortName + ".count()");
    print("\tdb." + shortName + ".copyTo(newColl) - duplicates collection by copying all documents to newColl; no indexes are copied.");
    print("\tdb." + shortName + ".convertToCapped(maxBytes) - calls {convertToCapped:'" + shortName + "', size:maxBytes}} command");
    print("\tdb." + shortName + ".dataSize()");
    print("\tdb." + shortName + ".distinct( key ) - eg. db." + shortName + ".distinct( 'x' )");
    print("\tdb." + shortName + ".drop() drop the collection");
    print("\tdb." + shortName + ".dropIndex(name)");
    print("\tdb." + shortName + ".dropIndexes()");
    print("\tdb." + shortName + ".ensureIndex(keypattern[,options]) - options is an object with these possible fields: name, unique, dropDups");
    print("\tdb." + shortName + ".reIndex()");
    print("\tdb." + shortName + ".find([query],[fields]) - query is an optional query filter. fields is optional set of fields to return.");
    print("\t                                              e.g. db." + shortName + ".find( {x:77} , {name:1, x:1} )");
    print("\tdb." + shortName + ".find(...).count()");
    print("\tdb." + shortName + ".find(...).limit(n)");
    print("\tdb." + shortName + ".find(...).skip(n)");
    print("\tdb." + shortName + ".find(...).sort(...)");
    print("\tdb." + shortName + ".findOne([query])");
    print("\tdb." + shortName + ".findAndModify( { update : ... , remove : bool [, query: {}, sort: {}, 'new': false] } )");
    print("\tdb." + shortName + ".getDB() get DB object associated with collection");
    print("\tdb." + shortName + ".getIndexes()");
    print("\tdb." + shortName + ".group( { key : ..., initial: ..., reduce : ...[, cond: ...] } )");
    print("\tdb." + shortName + ".insert(obj)");
    print("\tdb." + shortName + ".mapReduce( mapFunction , reduceFunction , <optional params> )");
    print("\tdb." + shortName + ".remove(query)");
    print("\tdb." + shortName + ".renameCollection( newName , <dropTarget> ) renames the collection.");
    print("\tdb." + shortName + ".runCommand( name , <options> ) runs a db command with the given name where the first param is the collection name");
    print("\tdb." + shortName + ".save(obj)");
    print("\tdb." + shortName + ".stats()");
    print("\tdb." + shortName + ".storageSize() - includes free space allocated to this collection");
    print("\tdb." + shortName + ".totalIndexSize() - size in bytes of all the indexes");
    print("\tdb." + shortName + ".totalSize() - storage allocated for all data and indexes");
    print("\tdb." + shortName + ".update(query, object[, upsert_bool, multi_bool]) - instead of two flags, you can pass an object with fields: upsert, multi");
    print("\tdb." + shortName + ".validate( <full> ) - SLOW");;
    print("\tdb." + shortName + ".getShardVersion() - only for use with sharding");
    print("\tdb." + shortName + ".getShardDistribution() - prints statistics about data distribution in the cluster");
    print("\tdb." + shortName + ".getSplitKeysForChunks( <maxChunkSize> ) - calculates split points over all chunks and returns splitter function");
    return __magicNoPrint;
}

DBCollection.prototype.getFullName = function(){
    return this._fullName;
}
DBCollection.prototype.getMongo = function(){
    return this._db.getMongo();
}
DBCollection.prototype.getDB = function(){
    return this._db;
}

DBCollection.prototype._dbCommand = function( cmd , params ){
    if ( typeof( cmd ) == "object" )
        return this._db._dbCommand( cmd );
    
    var c = {};
    c[cmd] = this.getName();
    if ( params )
        Object.extend( c , params );
    return this._db._dbCommand( c );    
}

DBCollection.prototype.runCommand = DBCollection.prototype._dbCommand;

DBCollection.prototype._massageObject = function( q ){
    if ( ! q )
        return {};

    var type = typeof q;

    if ( type == "function" )
        return { $where : q };

    if ( q.isObjectId )
        return { _id : q };

    if ( type == "object" )
        return q;

    if ( type == "string" ){
        if ( q.length == 24 )
            return { _id : q };

        return { $where : q };
    }

    throw "don't know how to massage : " + type;

}


DBCollection.prototype._validateObject = function( o ){
    if ( o._ensureSpecial && o._checkModify )
        throw "can't save a DBQuery object";
}

DBCollection._allowedFields = { $id : 1 , $ref : 1 , $db : 1 , $MinKey : 1, $MaxKey : 1 };

DBCollection.prototype._validateForStorage = function( o ){
    this._validateObject( o );
    for ( var k in o ){
        if ( k.indexOf( "." ) >= 0 ) {
            throw "can't have . in field names [" + k + "]" ;
        }

        if ( k.indexOf( "$" ) == 0 && ! DBCollection._allowedFields[k] ) {
            throw "field names cannot start with $ [" + k + "]";
        }

        if ( o[k] !== null && typeof( o[k] ) === "object" ) {
            this._validateForStorage( o[k] );
        }
    }
};


DBCollection.prototype.find = function( query , fields , limit , skip, batchSize, options ){
    return new DBQuery( this._mongo , this._db , this ,
                        this._fullName , this._massageObject( query ) , fields , limit , skip , batchSize , options || this.getQueryOptions() );
}

DBCollection.prototype.findOne = function( query , fields, options ){
    var cursor = this._mongo.find( this._fullName , this._massageObject( query ) || {} , fields , 
        -1 /* limit */ , 0 /* skip*/, 0 /* batchSize */ , options || this.getQueryOptions() /* options */ );
    if ( ! cursor.hasNext() )
        return null;
    var ret = cursor.next();
    if ( cursor.hasNext() ) throw "findOne has more than 1 result!";
    if ( ret.$err )
        throw "error " + tojson( ret );
    return ret;
}

DBCollection.prototype.insert = function( obj , _allow_dot ){
    if ( ! obj )
        throw "no object passed to insert!";
    if ( ! _allow_dot ) {
        this._validateForStorage( obj );
    }
    if ( typeof( obj._id ) == "undefined" && ! Array.isArray( obj ) ){
        var tmp = obj; // don't want to modify input
        obj = {_id: new ObjectId()};
        for (var key in tmp){
            obj[key] = tmp[key];
        }
    }
    this._db._initExtraInfo();
    this._mongo.insert( this._fullName , obj );
    this._lastID = obj._id;
    this._db._getExtraInfo("Inserted");
}

DBCollection.prototype.remove = function( t , justOne ){
    for ( var k in t ){
        if ( k == "_id" && typeof( t[k] ) == "undefined" ){
            throw "can't have _id set to undefined in a remove expression"
        }
    }
    this._db._initExtraInfo();
    this._mongo.remove( this._fullName , this._massageObject( t ) , justOne ? true : false );
    this._db._getExtraInfo("Removed");
}

DBCollection.prototype.update = function( query , obj , upsert , multi ){
    assert( query , "need a query" );
    assert( obj , "need an object" );

    var firstKey = null;
    for (var k in obj) { firstKey = k; break; }

    if (firstKey != null && firstKey[0] == '$') {
        // for mods we only validate partially, for example keys may have dots
        this._validateObject( obj );
    } else {
        // we're basically inserting a brand new object, do full validation
        this._validateForStorage( obj );
    }

    // can pass options via object for improved readability    
    if ( typeof(upsert) === 'object' ) {
        assert( multi === undefined, "Fourth argument must be empty when specifying upsert and multi with an object." );

        opts = upsert;
        multi = opts.multi;
        upsert = opts.upsert;
    }

    this._db._initExtraInfo();
    this._mongo.update( this._fullName , query , obj , upsert ? true : false , multi ? true : false );
    this._db._getExtraInfo("Updated");
}

DBCollection.prototype.save = function( obj ){
    if ( obj == null || typeof( obj ) == "undefined" ) 
        throw "can't save a null";

    if ( typeof( obj ) == "number" || typeof( obj) == "string" )
        throw "can't save a number or string"

    if ( typeof( obj._id ) == "undefined" ){
        obj._id = new ObjectId();
        return this.insert( obj );
    }
    else {
        return this.update( { _id : obj._id } , obj , true );
    }
}

DBCollection.prototype._genIndexName = function( keys ){
    var name = "";
    for ( var k in keys ){
        var v = keys[k];
        if ( typeof v == "function" )
            continue;
        
        if ( name.length > 0 )
            name += "_";
        name += k + "_";

        if ( typeof v == "number" )
            name += v;
    }
    return name;
}

DBCollection.prototype._indexSpec = function( keys, options ) {
    var ret = { ns : this._fullName , key : keys , name : this._genIndexName( keys ) };

    if ( ! options ){
    }
    else if ( typeof ( options ) == "string" )
        ret.name = options;
    else if ( typeof ( options ) == "boolean" )
        ret.unique = true;
    else if ( typeof ( options ) == "object" ){
        if ( options.length ){
            var nb = 0;
            for ( var i=0; i<options.length; i++ ){
                if ( typeof ( options[i] ) == "string" )
                    ret.name = options[i];
                else if ( typeof( options[i] ) == "boolean" ){
                    if ( options[i] ){
                        if ( nb == 0 )
                            ret.unique = true;
                        if ( nb == 1 )
                            ret.dropDups = true;
                    }
                    nb++;
                }
            }
        }
        else {
            Object.extend( ret , options );
        }
    }
    else {
        throw "can't handle: " + typeof( options );
    }
    /*
        return ret;

    var name;
    var nTrue = 0;
    
    if ( ! isObject( options ) ) {
        options = [ options ];
    }
    
    if ( options.length ){
        for( var i = 0; i < options.length; ++i ) {
            var o = options[ i ];
            if ( isString( o ) ) {
                ret.name = o;
            } else if ( typeof( o ) == "boolean" ) {
	        if ( o ) {
		    ++nTrue;
	        }
            }
        }
        if ( nTrue > 0 ) {
	    ret.unique = true;
        }
        if ( nTrue > 1 ) {
	    ret.dropDups = true;
        }
    }
*/
    return ret;
}

DBCollection.prototype.createIndex = function( keys , options ){
    var o = this._indexSpec( keys, options );
    this._db.getCollection( "system.indexes" ).insert( o , true );
}

DBCollection.prototype.ensureIndex = function( keys , options ){
    var name = this._indexSpec( keys, options ).name;
    this._indexCache = this._indexCache || {};
    if ( this._indexCache[ name ] ){
        return;
    }

    this.createIndex( keys , options );
    if ( this.getDB().getLastError() == "" ) {
	this._indexCache[name] = true;
    }
}

DBCollection.prototype.resetIndexCache = function(){
    this._indexCache = {};
}

DBCollection.prototype.reIndex = function() {
    return this._db.runCommand({ reIndex: this.getName() });
}

DBCollection.prototype.dropIndexes = function(){
    this.resetIndexCache();

    var res = this._db.runCommand( { deleteIndexes: this.getName(), index: "*" } );
    assert( res , "no result from dropIndex result" );
    if ( res.ok )
        return res;

    if ( res.errmsg.match( /not found/ ) )
        return res;

    throw "error dropping indexes : " + tojson( res );
}


DBCollection.prototype.drop = function(){
    if ( arguments.length > 0 )
        throw "drop takes no argument";
    this.resetIndexCache();
    var ret = this._db.runCommand( { drop: this.getName() } );
    if ( ! ret.ok ){
        if ( ret.errmsg == "ns not found" )
            return false;
        throw "drop failed: " + tojson( ret );
    }
    return true;
}

DBCollection.prototype.findAndModify = function(args){
    var cmd = { findandmodify: this.getName() };
    for (var key in args){
        cmd[key] = args[key];
    }

    var ret = this._db.runCommand( cmd );
    if ( ! ret.ok ){
        if (ret.errmsg == "No matching object found"){
            return null;
        }
        throw "findAndModifyFailed failed: " + tojson( ret.errmsg );
    }
    return ret.value;
}

DBCollection.prototype.renameCollection = function( newName , dropTarget ){
    return this._db._adminCommand( { renameCollection : this._fullName , 
                                     to : this._db._name + "." + newName , 
                                     dropTarget : dropTarget } )
}

DBCollection.prototype.validate = function(full) {
    var cmd = { validate: this.getName() };

    if (typeof(full) == 'object') // support arbitrary options here
        Object.extend(cmd, full);
    else
        cmd.full = full;

    var res = this._db.runCommand( cmd );

    if (typeof(res.valid) == 'undefined') {
        // old-style format just put everything in a string. Now using proper fields

        res.valid = false;

        var raw = res.result || res.raw;

        if ( raw ){
            var str = "-" + tojson( raw );
            res.valid = ! ( str.match( /exception/ ) || str.match( /corrupt/ ) );

            var p = /lastExtentSize:(\d+)/;
            var r = p.exec( str );
            if ( r ){
                res.lastExtentSize = Number( r[1] );
            }
        }
    }

    return res;
}

DBCollection.prototype.getShardVersion = function(){
    return this._db._adminCommand( { getShardVersion : this._fullName } );
}

DBCollection.prototype.getIndexes = function(){
    return this.getDB().getCollection( "system.indexes" ).find( { ns : this.getFullName() } ).toArray();
}

DBCollection.prototype.getIndices = DBCollection.prototype.getIndexes;
DBCollection.prototype.getIndexSpecs = DBCollection.prototype.getIndexes;

DBCollection.prototype.getIndexKeys = function(){
    return this.getIndexes().map(
        function(i){
            return i.key;
        }
    );
}


DBCollection.prototype.count = function( x ){
    return this.find( x ).count();
}

/**
 *  Drop free lists. Normally not used.
 *  Note this only does the collection itself, not the namespaces of its indexes (see cleanAll).
 */
DBCollection.prototype.clean = function() {
    return this._dbCommand( { clean: this.getName() } );
}



/**
 * <p>Drop a specified index.</p>
 *
 * <p>
 * Name is the name of the index in the system.indexes name field. (Run db.system.indexes.find() to
 *  see example data.)
 * </p>
 *
 * <p>Note :  alpha: space is not reclaimed </p>
 * @param {String} name of index to delete.
 * @return A result object.  result.ok will be true if successful.
 */
DBCollection.prototype.dropIndex =  function(index) {
    assert(index , "need to specify index to dropIndex" );

    if ( ! isString( index ) && isObject( index ) )
    	index = this._genIndexName( index );

    var res = this._dbCommand( "deleteIndexes" ,{ index: index } );
    this.resetIndexCache();
    return res;
}

DBCollection.prototype.copyTo = function( newName ){
    return this.getDB().eval(
        function( collName , newName ){
            var from = db[collName];
            var to = db[newName];
            to.ensureIndex( { _id : 1 } );
            var count = 0;

            var cursor = from.find();
            while ( cursor.hasNext() ){
                var o = cursor.next();
                count++;
                to.save( o );
            }

            return count;
        } , this.getName() , newName
    );
}

DBCollection.prototype.getCollection = function( subName ){
    return this._db.getCollection( this._shortName + "." + subName );
}

DBCollection.prototype.stats = function( scale ){
    return this._db.runCommand( { collstats : this._shortName , scale : scale } );
}

DBCollection.prototype.dataSize = function(){
    return this.stats().size;
}

DBCollection.prototype.storageSize = function(){
    return this.stats().storageSize;
}

DBCollection.prototype.totalIndexSize = function( verbose ){
    var stats = this.stats();
    if (verbose){
        for (var ns in stats.indexSizes){
            print( ns + "\t" + stats.indexSizes[ns] );
        }
    }
    return stats.totalIndexSize;
}


DBCollection.prototype.totalSize = function(){
    var total = this.storageSize();
    var mydb = this._db;
    var shortName = this._shortName;
    this.getIndexes().forEach(
        function( spec ){
            var coll = mydb.getCollection( shortName + ".$" + spec.name );
            var mysize = coll.storageSize();
            //print( coll + "\t" + mysize + "\t" + tojson( coll.validate() ) );
            total += coll.dataSize();
        }
    );
    return total;
}


DBCollection.prototype.convertToCapped = function( bytes ){
    if ( ! bytes )
        throw "have to specify # of bytes";
    return this._dbCommand( { convertToCapped : this._shortName , size : bytes } )
}

DBCollection.prototype.exists = function(){
    return this._db.system.namespaces.findOne( { name : this._fullName } );
}

DBCollection.prototype.isCapped = function(){
    var e = this.exists();
    return ( e && e.options && e.options.capped ) ? true : false;
}

DBCollection.prototype._distinct = function( keyString , query ){
    return this._dbCommand( { distinct : this._shortName , key : keyString , query : query || {} } );
    if ( ! res.ok )
        throw "distinct failed: " + tojson( res );
    return res.values;
}

DBCollection.prototype.distinct = function( keyString , query ){
    var res = this._distinct( keyString , query );
    if ( ! res.ok )
        throw "distinct failed: " + tojson( res );
    return res.values;
}


DBCollection.prototype.aggregate = function( ops ) {
    
    var arr = ops;
    
    if ( ! ops.length ) {
        arr = [];
        for ( var i=0; i<arguments.length; i++ ) {
            arr.push( arguments[i] )
        }
    }
    
    return this.runCommand( "aggregate" , { pipeline : arr } );
}

DBCollection.prototype.group = function( params ){
    params.ns = this._shortName;
    return this._db.group( params );
}

DBCollection.prototype.groupcmd = function( params ){
    params.ns = this._shortName;
    return this._db.groupcmd( params );
}

MapReduceResult = function( db , o ){
    Object.extend( this , o );
    this._o = o;
    this._keys = Object.keySet( o );
    this._db = db;
    if ( this.result != null ) {
        this._coll = this._db.getCollection( this.result );
    }
}

MapReduceResult.prototype._simpleKeys = function(){
    return this._o;
}

MapReduceResult.prototype.find = function(){
    if ( this.results )
        return this.results;
    return DBCollection.prototype.find.apply( this._coll , arguments );
}

MapReduceResult.prototype.drop = function(){
    if ( this._coll ) {
        return this._coll.drop();
    }
}

/**
* just for debugging really
*/
MapReduceResult.prototype.convertToSingleObject = function(){
    var z = {};
    var it = this.results != null ? this.results : this._coll.find();
    it.forEach( function(a){ z[a._id] = a.value; } );
    return z;
}

DBCollection.prototype.convertToSingleObject = function(valueField){
    var z = {};
    this.find().forEach( function(a){ z[a._id] = a[valueField]; } );
    return z;
}

/**
* @param optional object of optional fields;
*/
DBCollection.prototype.mapReduce = function( map , reduce , optionsOrOutString ){
    var c = { mapreduce : this._shortName , map : map , reduce : reduce };
    assert( optionsOrOutString , "need to supply an optionsOrOutString" )

    if ( typeof( optionsOrOutString ) == "string" )
        c["out"] = optionsOrOutString;
    else
        Object.extend( c , optionsOrOutString );

    var raw = this._db.runCommand( c );
    if ( ! raw.ok ){
        __mrerror__ = raw;
        throw "map reduce failed:" + tojson(raw);
    }
    return new MapReduceResult( this._db , raw );

}

DBCollection.prototype.toString = function(){
    return this.getFullName();
}

DBCollection.prototype.toString = function(){
    return this.getFullName();
}


DBCollection.prototype.tojson = DBCollection.prototype.toString;

DBCollection.prototype.shellPrint = DBCollection.prototype.toString;

DBCollection.autocomplete = function(obj){
    var colls = DB.autocomplete(obj.getDB());
    var ret = [];
    for (var i=0; i<colls.length; i++){
        var c = colls[i];
        if (c.length <= obj.getName().length) continue;
        if (c.slice(0,obj.getName().length+1) != obj.getName()+'.') continue;

        ret.push(c.slice(obj.getName().length+1));
    }
    return ret;
}


// Sharding additions

/* 
Usage :

mongo <mongos>
> load('path-to-file/shardingAdditions.js')
Loading custom sharding extensions...
true

> var collection = db.getMongo().getCollection("foo.bar")
> collection.getShardDistribution() // prints statistics related to the collection's data distribution

> collection.getSplitKeysForChunks() // generates split points for all chunks in the collection, based on the
                                     // default maxChunkSize or alternately a specified chunk size
> collection.getSplitKeysForChunks( 10 ) // Mb

> var splitter = collection.getSplitKeysForChunks() // by default, the chunks are not split, the keys are just
                                                    // found.  A splitter function is returned which will actually
                                                    // do the splits.
                                                    
> splitter() // ! Actually executes the splits on the cluster !
                                                    
*/

DBCollection.prototype.getShardDistribution = function(){

   var stats = this.stats()
   
   if( ! stats.sharded ){
       print( "Collection " + this + " is not sharded." )
       return
   }
   
   var config = this.getMongo().getDB("config")
       
   var numChunks = 0
   
   for( var shard in stats.shards ){
       
       var shardDoc = config.shards.findOne({ _id : shard })
       
       print( "\nShard " + shard + " at " + shardDoc.host ) 
       
       var shardStats = stats.shards[ shard ]
               
       var chunks = config.chunks.find({ _id : sh._collRE( this ), shard : shard }).toArray()
       
       numChunks += chunks.length
       
       var estChunkData = shardStats.size / chunks.length
       var estChunkCount = Math.floor( shardStats.count / chunks.length )
       
       print( " data : " + sh._dataFormat( shardStats.size ) +
              " docs : " + shardStats.count +
              " chunks : " +  chunks.length )
       print( " estimated data per chunk : " + sh._dataFormat( estChunkData ) )
       print( " estimated docs per chunk : " + estChunkCount )
       
   }
   
   print( "\nTotals" )
   print( " data : " + sh._dataFormat( stats.size ) +
          " docs : " + stats.count +
          " chunks : " +  numChunks )
   for( var shard in stats.shards ){
   
       var shardStats = stats.shards[ shard ]
       
       var estDataPercent = Math.floor( shardStats.size / stats.size * 10000 ) / 100
       var estDocPercent = Math.floor( shardStats.count / stats.count * 10000 ) / 100
       
       print( " Shard " + shard + " contains " + estDataPercent + "% data, " + estDocPercent + "% docs in cluster, " +
              "avg obj size on shard : " + sh._dataFormat( stats.shards[ shard ].avgObjSize ) )
   }
   
   print( "\n" )
   
}


DBCollection.prototype.getSplitKeysForChunks = function( chunkSize ){
       
   var stats = this.stats()
   
   if( ! stats.sharded ){
       print( "Collection " + this + " is not sharded." )
       return
   }
   
   var config = this.getMongo().getDB("config")
   
   if( ! chunkSize ){
       chunkSize = config.settings.findOne({ _id : "chunksize" }).value
       print( "Chunk size not set, using default of " + chunkSize + "Mb" )
   }
   else{
       print( "Using chunk size of " + chunkSize + "Mb" )
   }
    
   var shardDocs = config.shards.find().toArray()
   
   var allSplitPoints = {}
   var numSplits = 0    
   
   for( var i = 0; i < shardDocs.length; i++ ){
       
       var shardDoc = shardDocs[i]
       var shard = shardDoc._id
       var host = shardDoc.host
       var sconn = new Mongo( host )
       
       var chunks = config.chunks.find({ _id : sh._collRE( this ), shard : shard }).toArray()
       
       print( "\nGetting split points for chunks on shard " + shard + " at " + host )
               
       var splitPoints = []
       
       for( var j = 0; j < chunks.length; j++ ){
           var chunk = chunks[j]
           var result = sconn.getDB("admin").runCommand({ splitVector : this + "", min : chunk.min, max : chunk.max, maxChunkSize : chunkSize })
           if( ! result.ok ){
               print( " Had trouble getting split keys for chunk " + sh._pchunk( chunk ) + " :\n" )
               printjson( result )
           }
           else{
               splitPoints = splitPoints.concat( result.splitKeys )
               
               if( result.splitKeys.length > 0 )
                   print( " Added " + result.splitKeys.length + " split points for chunk " + sh._pchunk( chunk ) )
           }
       }
       
       print( "Total splits for shard " + shard + " : " + splitPoints.length )
       
       numSplits += splitPoints.length
       allSplitPoints[ shard ] = splitPoints
       
   }
   
   // Get most recent migration
   var migration = config.changelog.find({ what : /^move.*/ }).sort({ time : -1 }).limit( 1 ).toArray()
   if( migration.length == 0 ) 
       print( "\nNo migrations found in changelog." )
   else {
       migration = migration[0]
       print( "\nMost recent migration activity was on " + migration.ns + " at " + migration.time )
   }
   
   var admin = this.getMongo().getDB("admin") 
   var coll = this
   var splitFunction = function(){
       
       // Turn off the balancer, just to be safe
       print( "Turning off balancer..." )
       config.settings.update({ _id : "balancer" }, { $set : { stopped : true } }, true )
       print( "Sleeping for 30s to allow balancers to detect change.  To be extra safe, check config.changelog" +
              " for recent migrations." )
       sleep( 30000 )
              
       for( shard in allSplitPoints ){
           for( var i = 0; i < allSplitPoints[ shard ].length; i++ ){
               var splitKey = allSplitPoints[ shard ][i]
               print( "Splitting at " + tojson( splitKey ) )
               printjson( admin.runCommand({ split : coll + "", middle : splitKey }) )
           }
       }
       
       print( "Turning the balancer back on." )
       config.settings.update({ _id : "balancer" }, { $set : { stopped : false } } )
       sleep( 1 )
   }
   
   splitFunction.getSplitPoints = function(){ return allSplitPoints; }
   
   print( "\nGenerated " + numSplits + " split keys, run output function to perform splits.\n" +
          " ex : \n" + 
          "  > var splitter = <collection>.getSplitKeysForChunks()\n" +
          "  > splitter() // Execute splits on cluster !\n" )
       
   return splitFunction
   
}

DBCollection.prototype.setSlaveOk = function( value ) {
    if( value == undefined ) value = true;
    this._slaveOk = value;
}

DBCollection.prototype.getSlaveOk = function() {
    if (this._slaveOk != undefined) return this._slaveOk;
    return this._db.getSlaveOk();
}

DBCollection.prototype.getQueryOptions = function() {
    var options = 0;
    if (this.getSlaveOk()) options |= 4;
    return options;
}

