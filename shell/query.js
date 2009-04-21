// query.js

if ( typeof DBQuery == "undefined" ){
    print( "defining DBQuery" );
    DBQuery = function( mongo , db , collection , ns , query , fields , limit , skip ){
        
        this._mongo = mongo; // 0
        this._db = db; // 1
        this._collection = collection; // 2
        this._ns = ns; // 3
        
        this._query = query || {}; // 4
        this._fields = fields; // 5
        this._limit = limit || 0; // 6
        this._skip = skip || 0; // 7
        
        this._cursor = null;
        this._numReturned = 0;
        this._special = false;
    }
}


DBQuery.prototype.clone = function(){
    var q =  new DBQuery( this._mongo , this._db , this._collection , this._ns , 
        this._query , this._fields , 
        this._limit , this._skip );
    q._special = this._special;
    return q;
}

DBQuery.prototype._ensureSpecial = function(){
    if ( this._special )
        return;
    
    var n = { query : this._query };
    this._query = n;
    this._special = true;
}

DBQuery.prototype._checkModify = function(){
    if ( this._cursor )
        throw "query already executed";
}

DBQuery.prototype._exec = function(){
    if ( ! this._cursor ){
        assert.eq( 0 , this._numReturned );
        this._cursor = this._mongo.find( this._ns , this._query , this._fields , this._limit , this._skip );
        this._cursorSeen = 0;
    }
    return this._cursor;
}

DBQuery.prototype.limit = function( limit ){
    this._checkModify();
    this._limit = limit;
    return this;
}

DBQuery.prototype.skip = function( skip ){
    this._checkModify();
    this._skip = skip;
    return this;
}

DBQuery.prototype.hasNext = function(){
    this._exec();

    if ( this._limit > 0 && this._cursorSeen >= this._limit )
        return false;
    var o = this._cursor.hasNext();
    if ( o )
        this._cursorSeen++;
    return o;
}

DBQuery.prototype.next = function(){
    this._exec();
    
    var ret = this._cursor.next();
    if ( ret.$err && this._numReturned == 0 && ! this.hasNext() )
        throw "error: " + tojson( ret );

    this._numReturned++;
    return ret;
}

DBQuery.prototype.toArray = function(){
    if ( this._arr )
        return this._arr;
    
    var a = [];
    while ( this.hasNext() )
        a.push( this.next() );
    this._arr = a;
    return a;
}

DBQuery.prototype.count = function(){
    var cmd = { count: this._collection.getName() };
    if ( this._query ){
        if ( this._special )
            cmd.query = this._query.query;
        else 
            cmd.query = this._query;
    }
    cmd.fields = this._fields || {};
    
    var res = this._db.runCommand( cmd );
    if( res && res.n != null ) return res.n;
    throw "count failed: " + tojson( res );
}

/**
* iterative count - only for testing
*/
DBQuery.prototype.itcount = function(){
    var num = 0;
    while ( this.hasNext() ){
        num++;
        this.next();
    }
    return num;
}

DBQuery.prototype.length = function(){
    return this.toArray().length;
}

DBQuery.prototype.sort = function( sortBy ){
    this._ensureSpecial();
    this._query.orderby = sortBy;
    return this;
}

DBQuery.prototype.hint = function( hint ){
    this._ensureSpecial();
    this._query["$hint"] = hint;
    return this;
}


DBQuery.prototype.forEach = function( func ){
    while ( this.hasNext() )
        func( this.next() );
}

DBQuery.prototype.arrayAccess = function( idx ){
    return this.toArray()[idx];
}

DBQuery.prototype.explain = function(){
    var n = this.clone();
    n._ensureSpecial();
    n._query.$explain = true;
    return n.next();
}

DBQuery.prototype.shellPrint = function(){
    try {
        var n = 0;
        while ( this.hasNext() && n < 20 ){
            var s = tojson( this.next() );
            print( s );
            n++;
        }
        if ( this.hasNext() )
            print( "has more" );
    }
    catch ( e ){
        print( e );
    }
    
}

DBQuery.prototype.toString = function(){
    return "DBQuery: " + this._ns + " -> " + tojson( this.query );
}
