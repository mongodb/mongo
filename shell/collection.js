// collection.js


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

DBCollection.prototype.help = function(){
    print("DBCollection help");
    print("\tdb.foo.getDB() get DB object associated with collection");
    print("\tdb.foo.findOne(...)");
    print("\tdb.foo.find( [{..}] , [{...}]) - first parameters is an optional filter, 2nd paramter is optional and what fields to return.  db.foo.find( { x : 1 } , { name : 1 , x : 1 } )");
    print("\tdb.foo.find(...).sort(...)");
    print("\tdb.foo.find(...).limit(n)");
    print("\tdb.foo.find(...).skip(n)");
    print("\tdb.foo.find(...).count()");
    print("\tdb.foo.count()");
    print("\tdb.foo.group( { key : ..., initial: ..., reduce : ...[, cond: ...] } )");
    print("\tdb.foo.save(obj)");
    print("\tdb.foo.update(query, object[, upsert_bool])");
    print("\tdb.foo.remove(query)" );
    print("\tdb.foo.ensureIndex(keypattern,options) - options should be an object with these possible fields: name, unique, dropDups");
    print("\tdb.foo.dropIndexes()");
    print("\tdb.foo.dropIndex(name)");
    print("\tdb.foo.getIndexes()");
    print("\tdb.foo.drop() drop the collection");
    print("\tdb.foo.renameCollection( newName ) renames the collection");
    print("\tdb.foo.validate() - SLOW");
    print("\tdb.foo.stats()");
    print("\tdb.foo.dataSize()");
    print("\tdb.foo.storageSize() - includes free space allocated to this collection");
    print("\tdb.foo.totalIndexSize() - size in bytes of all the indexes");
    print("\tdb.foo.totalSize() - storage allocated for all data and indexe data");
}

DBCollection.prototype.getFullName = function(){
    return this._fullName;
}
DBCollection.prototype.getDB = function(){
    return this._db;
}

DBCollection.prototype._dbCommand = function( cmd ){
    return this._db._dbCommand( cmd );
}

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

DBCollection._allowedFields = { $id : 1 , $ref : 1 };

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


DBCollection.prototype.find = function( query , fields , limit , skip ){
    return new DBQuery( this._mongo , this._db , this ,
                        this._fullName , this._massageObject( query ) , fields , limit , skip );
}

DBCollection.prototype.findOne = function( query , fields ){
    var cursor = this._mongo.find( this._fullName , this._massageObject( query ) || {} , fields , -1 , 0  );
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
        throw "no object!";
    if ( ! _allow_dot ) {
        this._validateForStorage( obj );
    }
    return this._mongo.insert( this._fullName , obj );
}

DBCollection.prototype.remove = function( t ){
    this._mongo.remove( this._fullName , this._massageObject( t ) );
}

DBCollection.prototype.update = function( query , obj , upsert ){
    assert( query , "need a query" );
    assert( obj , "need an object" );
    this._validateObject( obj );
    return this._mongo.update( this._fullName , query , obj , upsert ? true : false );
}

DBCollection.prototype.save = function( obj ){
    if ( obj == null || typeof( obj ) == "undefined" ) 
        throw "can't save a null";

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
        if ( name.length > 0 )
            name += "_";
        name += k + "_";

        var v = keys[k];
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
        return false;
    }

    this.createIndex( keys , options );
    if ( this.getDB().getLastError() == "" ) {
	this._indexCache[name] = true;
    }
    return true;
}

DBCollection.prototype.resetIndexCache = function(){
    this._indexCache = {};
}

DBCollection.prototype.reIndex = function(){
    var specs = this.getIndexSpecs();
    this.dropIndexes();
    for ( var i = 0; i < specs.length; ++i ){
        this.ensureIndex( specs[i].key, [ specs[i].unique, specs[i].name ] );
    }
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
    this.resetIndexCache();
    return this._db.runCommand( { drop: this.getName() } );
}

DBCollection.prototype.renameCollection = function( newName ){
    return this._db._adminCommand( { renameCollection : this._fullName , to : this._db._name + "." + newName } ).ok;
}

DBCollection.prototype.validate = function() {
    var res = this._db.runCommand( { validate: this.getName() } );

    res.valid = false;

    if ( res.result ){
        var str = "-" + tojson( res.result );
        res.valid = ! ( str.match( /exception/ ) || str.match( /corrupt/ ) );

        var p = /lastExtentSize:(\d+)/;
        var r = p.exec( str );
        if ( r ){
            res.lastExtentSize = Number( r[1] );
        }
    }

    return res;
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

    var res = this._dbCommand( { deleteIndexes: this.getName(), index: index } );
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

DBCollection.prototype.stats = function(){
    return this._db.runCommand( { collstats : this._shortName } );
}

DBCollection.prototype.dataSize = function(){
    return this.stats().size;
}

DBCollection.prototype.storageSize = function(){
    return this.stats().storageSize;
}

DBCollection.prototype.totalIndexSize = function(){
    var total = 0;
    var mydb = this._db;
    var shortName = this._shortName;
    this.getIndexes().forEach(
        function( spec ){
            var coll = mydb.getCollection( shortName + ".$" + spec.name );
            var mysize = coll.dataSize();
            //print( coll + "\t" + mysize + "\t" + tojson( coll.validate() ) );
            total += coll.dataSize();
        }
    );
    return total;
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

DBCollection.prototype.distinct = function( keyString ){
    var res = this._dbCommand( { distinct : this._shortName , key : keyString } );
    if ( ! res.ok )
        throw "distinct failed: " + tojson( res );
    return res.values;
}

DBCollection.prototype.group = function( params ){
    params.ns = this._shortName;
    return this._db.group( params );
}

DBCollection.prototype.groupcmd = function( params ){
    params.ns = this._shortName;
    return this._db.groupcmd( params );
}

DBCollection.prototype.toString = function(){
    return this.getFullName();
}

DBCollection.prototype.shellPrint = DBCollection.prototype.toString;


