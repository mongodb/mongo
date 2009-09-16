// query.js

if ( typeof DBQuery == "undefined" ){
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
    print( "DBQuery probably won't have array access " );
}

DBQuery.prototype.help = function(){
    print( "DBQuery help" );
    print( "\t.sort( {...} )" )
    print( "\t.limit( n )" )
    print( "\t.skip( n )" )
    print( "\t.count()" )
    print( "\t.explain()" )
    print( "\t.forEach( func )" )
    print( "\t.map( func )" )

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
    return o;
}

DBQuery.prototype.next = function(){
    this._exec();
    
    var o = this._cursor.hasNext();
    if ( o )
        this._cursorSeen++;
    else
        throw "error hasNext: " + o;
    
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

DBQuery.prototype.countReturn = function(){
    var c = this.count();

    if ( this._skip )
        c = c - this._skip;

    if ( this._limit > 0 && this._limit < c )
        return this._limit;
    
    return c;
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

DBQuery.prototype.min = function( min ) {
    this._ensureSpecial();
    this._query["$min"] = min;
    return this;
}

DBQuery.prototype.max = function( max ) {
    this._ensureSpecial();
    this._query["$max"] = max;
    return this;
}

DBQuery.prototype.forEach = function( func ){
    while ( this.hasNext() )
        func( this.next() );
}

DBQuery.prototype.map = function( func ){
    var a = [];
    while ( this.hasNext() )
        a.push( func( this.next() ) );
    return a;
}

DBQuery.prototype.arrayAccess = function( idx ){
    return this.toArray()[idx];
}

DBQuery.prototype.explain = function(){
    var n = this.clone();
    n._ensureSpecial();
    n._query.$explain = true;
    n._limit = n._limit * -1;
    return n.next();
}

DBQuery.prototype.snapshot = function(){
    this._ensureSpecial();
    this._query.$snapshot = true;
    return this;
 }

DBQuery.prototype.shellPrint = function(){
    try {
        var n = 0;
        while ( this.hasNext() && n < 20 ){
            var s = tojson( this.next() );
            print( s );
            n++;
        }
        if ( this.hasNext() ){
            print( "has more" );
            ___it___  = this;
        }
        else {
            ___it___  = null;
        }
    }
    catch ( e ){
        print( e );
    }
    
}

DBQuery.prototype.toString = function(){
    return "DBQuery: " + this._ns + " -> " + tojson( this.query );
}
