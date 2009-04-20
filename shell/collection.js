// collection.js


if ( ( typeof  DBCollection ) == "undefined" ){
    print( "defined DBCollection" );

    DBCollection = function( mongo , db , shortName , fullName ){
        this._mongo = mongo;
        this._db = db;
        this._shortName = shortName;
        this._fullName = fullName;
        
        assert( this._mongo , "no mongo" );
        assert( this._db , "no db" );
        assert( this._shortName , "no shortName" );
        assert( this._fullName , "no fullName" );
    }
}

DBCollection.prototype.getName = function(){
    return this._shortName;
}

DBCollection.prototype.help = function(){
    print("DBCollection help");
    print("\tdb.foo.getDB() get DB object associated with collection");
    print("\tdb.foo.findOne(...)");
    print("\tdb.foo.find(...)");
    print("\tdb.foo.find(...).sort(...)");
    print("\tdb.foo.find(...).limit(n)");
    print("\tdb.foo.find(...).skip(n)");
    print("\tdb.foo.find(...).count()");
    print("\tdb.foo.count()");
    print("\tdb.foo.save(obj)");
    print("\tdb.foo.update(query, object[, upsert_bool])");
    print("\tdb.foo.ensureIndex(keypattern)");
    print("\tdb.foo.dropIndexes()");
    print("\tdb.foo.dropIndex(name)");
    print("\tdb.foo.getIndexes()");
    print("\tdb.foo.drop() drop the collection");
    print("\tdb.foo.validate()");
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

DBCollection.prototype._validateForStorage = function( o ){
    for ( var k in o ){
        if ( k.indexOf( "." ) >= 0 )
            throw "can't have . in field names [" + k + "]" ;
    }
}

DBCollection.prototype.find = function( query , fields , limit , skip ){
    return new DBQuery( this._mongo , this._db , this , 
                        this._fullName , this._massageObject( query ) , fields , limit , skip );
}

DBCollection.prototype.findOne = function( query , fields ){
    var cursor = this._mongo.find( this._fullName , this._massageObject( query ) || {} , fields , 1 , 0  );
    if ( ! cursor.hasNext() )
        return null;
    var ret = cursor.next();
    if ( cursor.hasNext() ) throw "something is wrong";
    if ( ret.$err )
        throw "error " + tojson( ret );
    return ret;
}

DBCollection.prototype.insert = function( obj ){
    if ( ! obj )
        throw "no object!";
    this._validateForStorage( obj );
    return this._mongo.insert( this._fullName , obj );
}

DBCollection.prototype.remove = function( t ){
    this._mongo.remove( this._fullName , this._massageObject( t ) );
}

DBCollection.prototype.update = function( query , obj , upsert ){
    assert( query , "need a query" );
    assert( obj , "need an object" );
    return this._mongo.update( this._fullName , query , obj , upsert ? true : false );
}

DBCollection.prototype.save = function( obj ){
    if ( ! obj._id ){
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
    var name;
    var unique = false;
    if ( !isObject( options ) ) {
        options = [ options ];
    }
    for( var i = 0; i < options.length; ++i ) {
        var o = options[ i ];
        if ( isString( o ) ) {
            name = o;
        } else if ( typeof( o ) == "boolean" ) {
            unique = o;
        }
    }
    name = name || this._genIndexName( keys );
    var ret = { ns : this._fullName , key : keys , name : name };
    if ( unique == true ) {
        ret.unique = true;
    }
    return ret;
}

DBCollection.prototype.createIndex = function( keys , options ){
    var o = this._indexSpec( keys, options );
    this._db.getCollection( "system.indexes" ).insert( o );
}

DBCollection.prototype.ensureIndex = function( keys , options ){
    var name = this._indexSpec( keys, options ).name;
    this._indexCache = this._indexCache || {};
    if ( this._indexCache[ name ] )
        return false;

    this.createIndex( keys , options );
    this._indexCache[name] = true;
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
    return this.getDB().getCollection( "system.indexes" ).find( { ns : this.getFullName() } );
}

DBCollection.prototype.getIndexSpecs = function(){
    return this.getIndexes().toArray().map( 
        function(i){
            return i;
        }
    );
}


DBCollection.prototype.count = function(){
    return this.find().count();
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

DBCollection.prototype.toString = function(){
    return this.getFullName();
}
