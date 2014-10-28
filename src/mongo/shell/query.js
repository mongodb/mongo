// query.js

if ( typeof DBQuery == "undefined" ){
    DBQuery = function( mongo , db , collection , ns , query , fields , limit , skip , batchSize , options ){
        
        this._mongo = mongo; // 0
        this._db = db; // 1
        this._collection = collection; // 2
        this._ns = ns; // 3
        
        this._query = query || {}; // 4
        this._fields = fields; // 5
        this._limit = limit || 0; // 6
        this._skip = skip || 0; // 7
        this._batchSize = batchSize || 0;
        this._options = options || 0;

        this._cursor = null;
        this._numReturned = 0;
        this._special = false;
        this._prettyShell = false;
    }
    print( "DBQuery probably won't have array access " );
}

DBQuery.prototype.help = function () {
    print("find() modifiers")
    print("\t.sort( {...} )")
    print("\t.limit( n )")
    print("\t.skip( n )")
    print("\t.count(applySkipLimit) - total # of objects matching query. by default ignores skip,limit")
    print("\t.size() - total # of objects cursor would return, honors skip,limit")
    print("\t.explain([verbose])")
    print("\t.hint(...)")
    print("\t.addOption(n) - adds op_query options -- see wire protocol")
    print("\t._addSpecial(name, value) - http://dochub.mongodb.org/core/advancedqueries#AdvancedQueries-Metaqueryoperators")
    print("\t.batchSize(n) - sets the number of docs to return per getMore")
    print("\t.showDiskLoc() - adds a $diskLoc field to each returned object")
    print("\t.min(idxDoc)")
    print("\t.max(idxDoc)")
    print("\t.comment(comment)")
    print("\t.snapshot()")
    print("\t.readPref(mode, tagset)")
    
    print("\nCursor methods");
    print("\t.toArray() - iterates through docs and returns an array of the results")
    print("\t.forEach( func )")
    print("\t.map( func )")
    print("\t.hasNext()")
    print("\t.next()")
    print("\t.objsLeftInBatch() - returns count of docs left in current batch (when exhausted, a new getMore will be issued)")
    print("\t.itcount() - iterates through documents and counts them")
    print("\t.getQueryPlan() - get query plans associated with shape. To get more info on query plans, " +
          "call getQueryPlan().help().");
    print("\t.pretty() - pretty print each document, possibly over multiple lines")
}

DBQuery.prototype.clone = function(){
    var q =  new DBQuery( this._mongo , this._db , this._collection , this._ns , 
        this._query , this._fields , 
        this._limit , this._skip , this._batchSize , this._options );
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
        throw Error("query already executed");
}

DBQuery.prototype._exec = function(){
    if ( ! this._cursor ){
        assert.eq( 0 , this._numReturned );
        this._cursor = this._mongo.find( this._ns , this._query , this._fields , this._limit , this._skip , this._batchSize , this._options );
        this._cursorSeen = 0;
    }
    return this._cursor;
}

/**
 * Helper for _convertToCommand() which constructs the "options" part of the find command.
 */
DBQuery.prototype._buildCmdOptions = function() {
    var options = {};

    if (this._query.$comment) {
        options["comment"] = this._query.$comment;
    }

    if (this._query.$maxScan) {
        options["maxScan"] = this._query.$maxScan;
    }

    if (this._query.$maxTimeMS) {
        options["maxTimeMS"] = this._query.$maxTimeMS;
    }

    if (this._query.$max) {
        options["max"] = this._query.$max;
    }

    if (this._query.$min) {
        options["min"] = this._query.$min;
    }

    if (this._query.$returnKey) {
        options["returnKey"] = this._query.$returnKey;
    }

    if (this._query.$showDiskLoc) {
        options["showDiskLoc"] = this._query.$showDiskLoc;
    }

    if (this._query.$snapshot) {
        options["snapshot"] = this._query.$snapshot;
    }

    if ((this._options & DBQuery.Option.tailable) != 0) {
        options["tailable"] = true;
    }

    if ((this._options & DBQuery.Option.slaveOk) != 0) {
        options["slaveOk"] = true;
    }

    if ((this._options & DBQuery.Option.oplogReplay) != 0) {
        options["oplogReplay"] = true;
    }

    if ((this._options & DBQuery.Option.noTimeout) != 0) {
        options["noCursorTimeout"] = true;
    }

    if ((this._options & DBQuery.Option.awaitData) != 0) {
        options["awaitData"] = true;
    }

    if ((this._options & DBQuery.Option.exhaust) != 0) {
        options["exhaust"] = true;
    }

    if ((this._options & DBQuery.Option.partial) != 0) {
        options["partial"] = true;
    }

    return options;
}

/**
 * Internal helper used to convert this cursor into the format required by the find command.
 */
DBQuery.prototype._convertToCommand = function() {
    var cmd = {};

    cmd["find"] = this._collection.getName();

    if (this._special) {
        if (this._query.query) {
            cmd["filter"] = this._query.query;
        }
    }
    else if (this._query) {
        cmd["filter"] = this._query;
    }

    if (this._skip) {
        cmd["skip"] = this._skip
    }

    if (this._batchSize) {
        cmd["batchSize"] = this._batchSize || 101;
    }

    if (this._limit) {
        if (this._limit < 0) {
            cmd["limit"] = -this._limit;
            cmd["singleBatch"] = true;
        }
        else {
            cmd["limit"] = this._limit;
            cmd["singleBatch"] = false;
        }
    }

    if (this._query.orderby) {
        cmd["sort"] = this._query.orderby;
    }

    if (this._fields) {
        cmd["projection"] = this._fields;
    }

    if (this._query.$hint) {
        cmd["hint"] = this._query.$hint;
    }

    if (this._query.$readPreference) {
       cmd["$readPreference"] = this._query.$readPreference;
    }

    cmd["options"] = this._buildCmdOptions();

    return cmd;
}

DBQuery.prototype.limit = function( limit ){
    this._checkModify();
    this._limit = limit;
    return this;
}

DBQuery.prototype.batchSize = function( batchSize ){
    this._checkModify();
    this._batchSize = batchSize;
    return this;
}


DBQuery.prototype.addOption = function( option ){
    this._options |= option;
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
        throw Error( "error hasNext: " + o );
    
    var ret = this._cursor.next();
    if ( ret.$err )
        throw Error( "error: " + tojson( ret ) );

    this._numReturned++;
    return ret;
}

DBQuery.prototype.objsLeftInBatch = function(){
    this._exec();

    var ret = this._cursor.objsLeftInBatch();
    if ( ret.$err )
        throw Error( "error: " + tojson( ret ) );

    return ret;
}

DBQuery.prototype.readOnly = function(){
    this._exec();
    this._cursor.readOnly();
    return this;
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

DBQuery.prototype._convertToCountCmd = function( applySkipLimit ) {
    var cmd = { count: this._collection.getName() };

    if ( this._query ) {
        if ( this._special ) {
            cmd.query = this._query.query;
            if ( this._query.$maxTimeMS ) {
                cmd.maxTimeMS = this._query.$maxTimeMS;
            }
            if ( this._query.$hint ) {
                cmd.hint = this._query.$hint;
            }
        }
        else {
            cmd.query = this._query;
        }
    }
    cmd.fields = this._fields || {};

    if ( applySkipLimit ) {
        if ( this._limit )
            cmd.limit = this._limit;
        if ( this._skip )
            cmd.skip = this._skip;
    }

    return cmd;
}

DBQuery.prototype.count = function( applySkipLimit ) {
    var cmd = this._convertToCountCmd( applySkipLimit );

    var res = this._db.runCommand( cmd );
    if( res && res.n != null ) return res.n;
    throw Error( "count failed: " + tojson( res ) );
}

DBQuery.prototype.size = function(){
    return this.count( true );
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

DBQuery.prototype._addSpecial = function( name , value ){
    this._ensureSpecial();
    this._query[name] = value;
    return this;
}

DBQuery.prototype.sort = function( sortBy ){
    return this._addSpecial( "orderby" , sortBy );
}

DBQuery.prototype.hint = function( hint ){
    return this._addSpecial( "$hint" , hint );
}

DBQuery.prototype.min = function( min ) {
    return this._addSpecial( "$min" , min );
}

DBQuery.prototype.max = function( max ) {
    return this._addSpecial( "$max" , max );
}

DBQuery.prototype.showDiskLoc = function() {
    return this._addSpecial( "$showDiskLoc" , true );
}

DBQuery.prototype.maxTimeMS = function( maxTimeMS ) {
    return this._addSpecial( "$maxTimeMS" , maxTimeMS );
}

/**
 * Sets the read preference for this cursor.
 * 
 * @param mode {string} read preference mode to use.
 * @param tagSet {Array.<Object>} optional. The list of tags to use, order matters.
 *     Note that this object only keeps a shallow copy of this array.
 * 
 * @return this cursor
 */
DBQuery.prototype.readPref = function( mode, tagSet ) {
    var readPrefObj = {
        mode: mode
    };

    if ( tagSet ){
        readPrefObj.tags = tagSet;
    }

    return this._addSpecial( "$readPreference", readPrefObj );
};

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

DBQuery.prototype.comment = function (comment) {
    return this._addSpecial( "$comment" , comment );
}

DBQuery.prototype.explain = function (verbose) {
    var explainQuery = new DBExplainQuery(this, verbose);
    return explainQuery.finish();
}

DBQuery.prototype.snapshot = function(){
    return this._addSpecial( "$snapshot" , true );
}

DBQuery.prototype.pretty = function(){
    this._prettyShell = true;
    return this;
}

DBQuery.prototype.shellPrint = function(){
    try {
        var start = new Date().getTime();
        var n = 0;
        while ( this.hasNext() && n < DBQuery.shellBatchSize ){
            var s = this._prettyShell ? tojson( this.next() ) : tojson( this.next() , "" , true );
            print( s );
            n++;
        }
        if (typeof _verboseShell !== 'undefined' && _verboseShell) {
            var time = new Date().getTime() - start;
            print("Fetched " + n + " record(s) in " + time + "ms");
        }
         if ( this.hasNext() ){
            print( "Type \"it\" for more" );
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

/**
 * Returns a QueryPlan for the query.
 */
DBQuery.prototype.getQueryPlan = function() {
    return new QueryPlan( this );
}

DBQuery.prototype.toString = function(){
    return "DBQuery: " + this._ns + " -> " + tojson( this._query );
}

DBQuery.shellBatchSize = 20;

/**
 * Query option flag bit constants.
 * @see http://dochub.mongodb.org/core/mongowireprotocol#MongoWireProtocol-OPQUERY
 */
DBQuery.Option = {
    tailable: 0x2,
    slaveOk: 0x4,
    oplogReplay: 0x8,
    noTimeout: 0x10,
    awaitData: 0x20,
    exhaust: 0x40,
    partial: 0x80
};

function DBCommandCursor(mongo, cmdResult, batchSize) {
    assert.commandWorked(cmdResult)
    this._firstBatch = cmdResult.cursor.firstBatch.reverse(); // modifies input to allow popping
    this._cursor = mongo.cursorFromId(cmdResult.cursor.ns, cmdResult.cursor.id, batchSize);
}

DBCommandCursor.prototype = {};
DBCommandCursor.prototype.hasNext = function() {
    return this._firstBatch.length || this._cursor.hasNext();
}
DBCommandCursor.prototype.next = function() {
    if (this._firstBatch.length) {
        // $err wouldn't be in _firstBatch since ok was true.
        return this._firstBatch.pop();
    }
    else {
        var ret = this._cursor.next();
        if ( ret.$err )
            throw Error( "error: " + tojson( ret ) );
        return ret;
    }
}
DBCommandCursor.prototype.objsLeftInBatch = function() {
    if (this._firstBatch.length) {
        return this._firstBatch.length;
    }
    else {
        return this._cursor.objsLeftInBatch();
    }
}

DBCommandCursor.prototype.help = function () {
    // This is the same as the "Cursor Methods" section of DBQuery.help().
    print("\nCursor methods");
    print("\t.toArray() - iterates through docs and returns an array of the results")
    print("\t.forEach( func )")
    print("\t.map( func )")
    print("\t.hasNext()")
    print("\t.next()")
    print("\t.objsLeftInBatch() - returns count of docs left in current batch (when exhausted, a new getMore will be issued)")
    print("\t.itcount() - iterates through documents and counts them")
    print("\t.pretty() - pretty print each document, possibly over multiple lines")
}

// Copy these methods from DBQuery
DBCommandCursor.prototype.toArray = DBQuery.prototype.toArray
DBCommandCursor.prototype.forEach = DBQuery.prototype.forEach
DBCommandCursor.prototype.map = DBQuery.prototype.map
DBCommandCursor.prototype.itcount = DBQuery.prototype.itcount
DBCommandCursor.prototype.shellPrint = DBQuery.prototype.shellPrint
DBCommandCursor.prototype.pretty = DBQuery.prototype.pretty

/**
 * QueryCache
 * Holds a reference to the cursor.
 * Proxy for planCache* query shape-specific commands.
 */
if ( ( typeof  QueryPlan ) == "undefined" ){
    QueryPlan = function( cursor ){
        this._cursor = cursor;
    }
}

/**
 * Name of QueryPlan.
 * Same as collection.
 */
QueryPlan.prototype.getName = function() {
    return this._cursor._collection.getName();
}

/**
 * tojson prints the name of the collection
 */

QueryPlan.prototype.tojson = function(indent, nolint) {
    return tojson(this.getPlans());
}

/**
 * Displays help for a PlanCache object.
 */
QueryPlan.prototype.help = function () {
    var shortName = this.getName();
    print("QueryPlan help");
    print("\t.help() - show QueryPlan help");
    print("\t.clearPlans() - drops query shape from plan cache");
    print("\t.getPlans() - displays the cached plans for a query shape");
    return __magicNoPrint;
}

/**
 * List plans for a query shape.
 */
QueryPlan.prototype.getPlans = function() {
    return this._cursor._collection.getPlanCache().getPlansByQuery(this._cursor);
}

/**
 * Drop query shape from the plan cache.
 */
QueryPlan.prototype.clearPlans = function() {
    this._cursor._collection.getPlanCache().clearPlansByQuery(this._cursor);
    return;
}
