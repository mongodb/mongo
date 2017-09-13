// query.js

if (typeof DBQuery == "undefined") {
    DBQuery = function(mongo, db, collection, ns, query, fields, limit, skip, batchSize, options) {

        this._mongo = mongo;            // 0
        this._db = db;                  // 1
        this._collection = collection;  // 2
        this._ns = ns;                  // 3

        this._query = query || {};  // 4
        this._fields = fields;      // 5
        this._limit = limit || 0;   // 6
        this._skip = skip || 0;     // 7
        this._batchSize = batchSize || 0;
        this._options = options || 0;

        this._cursor = null;
        this._numReturned = 0;
        this._special = false;
        this._prettyShell = false;
    };
    print("DBQuery probably won't have array access ");
}

DBQuery.prototype.help = function() {
    print("find(<predicate>, <projection>) modifiers");
    print("\t.sort({...})");
    print("\t.limit(<n>)");
    print("\t.skip(<n>)");
    print("\t.batchSize(<n>) - sets the number of docs to return per getMore");
    print("\t.collation({...})");
    print("\t.hint({...})");
    print("\t.readConcern(<level>)");
    print("\t.readPref(<mode>, <tagset>)");
    print(
        "\t.count(<applySkipLimit>) - total # of objects matching query. by default ignores skip,limit");
    print("\t.size() - total # of objects cursor would return, honors skip,limit");
    print(
        "\t.explain(<verbosity>) - accepted verbosities are {'queryPlanner', 'executionStats', 'allPlansExecution'}");
    print("\t.min({...})");
    print("\t.max({...})");
    print("\t.maxScan(<n>)");
    print("\t.maxTimeMS(<n>)");
    print("\t.comment(<comment>)");
    print("\t.snapshot()");
    print("\t.tailable(<isAwaitData>)");
    print("\t.noCursorTimeout()");
    print("\t.allowPartialResults()");
    print("\t.returnKey()");
    print("\t.showRecordId() - adds a $recordId field to each returned object");

    print("\nCursor methods");
    print("\t.toArray() - iterates through docs and returns an array of the results");
    print("\t.forEach(<func>)");
    print("\t.map(<func>)");
    print("\t.hasNext()");
    print("\t.next()");
    print("\t.close()");
    print(
        "\t.objsLeftInBatch() - returns count of docs left in current batch (when exhausted, a new getMore will be issued)");
    print("\t.itcount() - iterates through documents and counts them");
    print(
        "\t.getQueryPlan() - get query plans associated with shape. To get more info on query plans, " +
        "call getQueryPlan().help().");
    print("\t.pretty() - pretty print each document, possibly over multiple lines");
};

DBQuery.prototype.clone = function() {
    var q = new DBQuery(this._mongo,
                        this._db,
                        this._collection,
                        this._ns,
                        this._query,
                        this._fields,
                        this._limit,
                        this._skip,
                        this._batchSize,
                        this._options);
    q._special = this._special;
    return q;
};

DBQuery.prototype._ensureSpecial = function() {
    if (this._special)
        return;

    var n = {query: this._query};
    this._query = n;
    this._special = true;
};

DBQuery.prototype._checkModify = function() {
    if (this._cursor)
        throw Error("query already executed");
};

DBQuery.prototype._canUseFindCommand = function() {
    // Since runCommand() is implemented by running a findOne() against the $cmd collection, we have
    // to make sure that we don't try to run a find command against the $cmd collection.
    //
    // We also forbid queries with the exhaust option from running as find commands, because the
    // find command does not support exhaust.
    return (this._collection.getName().indexOf("$cmd") !== 0) &&
        (this._options & DBQuery.Option.exhaust) === 0;
};

DBQuery.prototype._exec = function() {
    if (!this._cursor) {
        assert.eq(0, this._numReturned);
        this._cursorSeen = 0;

        if (this._mongo.useReadCommands() && this._canUseFindCommand()) {
            var canAttachReadPref = true;
            var findCmd = this._convertToCommand(canAttachReadPref);
            var cmdRes = this._db.runReadCommand(findCmd, null, this._options);
            this._cursor = new DBCommandCursor(cmdRes._mongo, cmdRes, this._batchSize);
        } else {
            if (this._special && this._query.readConcern) {
                throw new Error("readConcern requires use of read commands");
            }

            if (this._special && this._query.collation) {
                throw new Error("collation requires use of read commands");
            }

            this._cursor = this._mongo.find(this._ns,
                                            this._query,
                                            this._fields,
                                            this._limit,
                                            this._skip,
                                            this._batchSize,
                                            this._options);
        }
    }
    return this._cursor;
};

/**
 * Internal helper used to convert this cursor into the format required by the find command.
 *
 * If canAttachReadPref is true, may attach a read preference to the resulting command using the
 * "wrapped form": { $query: { <cmd>: ... }, $readPreference: { ... } }.
 */
DBQuery.prototype._convertToCommand = function(canAttachReadPref) {
    var cmd = {};

    cmd["find"] = this._collection.getName();

    if (this._special) {
        if (this._query.query) {
            cmd["filter"] = this._query.query;
        }
    } else if (this._query) {
        cmd["filter"] = this._query;
    }

    if (this._skip) {
        cmd["skip"] = this._skip;
    }

    if (this._batchSize) {
        if (this._batchSize < 0) {
            cmd["batchSize"] = -this._batchSize;
            cmd["singleBatch"] = true;
        } else {
            cmd["batchSize"] = this._batchSize;
        }
    }

    if (this._limit) {
        if (this._limit < 0) {
            cmd["limit"] = -this._limit;
            cmd["singleBatch"] = true;
        } else {
            cmd["limit"] = this._limit;
            cmd["singleBatch"] = false;
        }
    }

    if ("orderby" in this._query) {
        cmd["sort"] = this._query.orderby;
    }

    if (this._fields) {
        cmd["projection"] = this._fields;
    }

    if ("$hint" in this._query) {
        cmd["hint"] = this._query.$hint;
    }

    if ("$comment" in this._query) {
        cmd["comment"] = this._query.$comment;
    }

    if ("$maxScan" in this._query) {
        cmd["maxScan"] = this._query.$maxScan;
    }

    if ("$maxTimeMS" in this._query) {
        cmd["maxTimeMS"] = this._query.$maxTimeMS;
    }

    if ("$max" in this._query) {
        cmd["max"] = this._query.$max;
    }

    if ("$min" in this._query) {
        cmd["min"] = this._query.$min;
    }

    if ("$returnKey" in this._query) {
        cmd["returnKey"] = this._query.$returnKey;
    }

    if ("$showDiskLoc" in this._query) {
        cmd["showRecordId"] = this._query.$showDiskLoc;
    }

    if ("$snapshot" in this._query) {
        cmd["snapshot"] = this._query.$snapshot;
    }

    if ("readConcern" in this._query) {
        cmd["readConcern"] = this._query.readConcern;
    }

    if ("collation" in this._query) {
        cmd["collation"] = this._query.collation;
    }

    if ((this._options & DBQuery.Option.tailable) != 0) {
        cmd["tailable"] = true;
    }

    if ((this._options & DBQuery.Option.oplogReplay) != 0) {
        cmd["oplogReplay"] = true;
    }

    if ((this._options & DBQuery.Option.noTimeout) != 0) {
        cmd["noCursorTimeout"] = true;
    }

    if ((this._options & DBQuery.Option.awaitData) != 0) {
        cmd["awaitData"] = true;
    }

    if ((this._options & DBQuery.Option.partial) != 0) {
        cmd["allowPartialResults"] = true;
    }

    if (canAttachReadPref) {
        // If there is a readPreference, use the wrapped command form.
        if ("$readPreference" in this._query) {
            var prefObj = this._query.$readPreference;
            cmd = this._db._attachReadPreferenceToCommand(cmd, prefObj);
        }
    }

    return cmd;
};

DBQuery.prototype.limit = function(limit) {
    this._checkModify();
    this._limit = limit;
    return this;
};

DBQuery.prototype.batchSize = function(batchSize) {
    this._checkModify();
    this._batchSize = batchSize;
    return this;
};

DBQuery.prototype.addOption = function(option) {
    this._options |= option;
    return this;
};

DBQuery.prototype.skip = function(skip) {
    this._checkModify();
    this._skip = skip;
    return this;
};

DBQuery.prototype.hasNext = function() {
    this._exec();

    if (this._limit > 0 && this._cursorSeen >= this._limit) {
        this._cursor.close();
        return false;
    }
    var o = this._cursor.hasNext();
    return o;
};

DBQuery.prototype.next = function() {
    this._exec();

    var o = this._cursor.hasNext();
    if (o)
        this._cursorSeen++;
    else
        throw Error("error hasNext: " + o);

    var ret = this._cursor.next();
    if (ret.$err) {
        throw _getErrorWithCode(ret, "error: " + tojson(ret));
    }

    this._numReturned++;
    return ret;
};

DBQuery.prototype.objsLeftInBatch = function() {
    this._exec();

    var ret = this._cursor.objsLeftInBatch();
    if (ret.$err)
        throw _getErrorWithCode(ret, "error: " + tojson(ret));

    return ret;
};

DBQuery.prototype.readOnly = function() {
    this._exec();
    this._cursor.readOnly();
    return this;
};

DBQuery.prototype.toArray = function() {
    if (this._arr)
        return this._arr;

    var a = [];
    while (this.hasNext())
        a.push(this.next());
    this._arr = a;
    return a;
};

DBQuery.prototype._convertToCountCmd = function(applySkipLimit) {
    var cmd = {count: this._collection.getName()};

    if (this._query) {
        if (this._special) {
            cmd.query = this._query.query;
            if (this._query.$maxTimeMS) {
                cmd.maxTimeMS = this._query.$maxTimeMS;
            }
            if (this._query.$hint) {
                cmd.hint = this._query.$hint;
            }
            if (this._query.readConcern) {
                cmd.readConcern = this._query.readConcern;
            }
            if (this._query.collation) {
                cmd.collation = this._query.collation;
            }
        } else {
            cmd.query = this._query;
        }
    }
    cmd.fields = this._fields || {};

    if (applySkipLimit) {
        if (this._limit)
            cmd.limit = this._limit;
        if (this._skip)
            cmd.skip = this._skip;
    }

    return cmd;
};

DBQuery.prototype.count = function(applySkipLimit) {
    var cmd = this._convertToCountCmd(applySkipLimit);

    var res = this._db.runReadCommand(cmd);
    if (res && res.n != null)
        return res.n;
    throw _getErrorWithCode(res, "count failed: " + tojson(res));
};

DBQuery.prototype.size = function() {
    return this.count(true);
};

DBQuery.prototype.countReturn = function() {
    var c = this.count();

    if (this._skip)
        c = c - this._skip;

    if (this._limit > 0 && this._limit < c)
        return this._limit;

    return c;
};

/**
* iterative count - only for testing
*/
DBQuery.prototype.itcount = function() {
    var num = 0;

    // Track how many bytes we've used this cursor to iterate iterated.  This function can be called
    // with some very large cursors.  SpiderMonkey appears happy to allow these objects to
    // accumulate, so regular gc() avoids an overly large memory footprint.
    //
    // TODO: migrate this function into c++
    var bytesSinceGC = 0;

    while (this.hasNext()) {
        num++;
        var nextDoc = this.next();
        bytesSinceGC += Object.bsonsize(nextDoc);

        // Garbage collect every 10 MB.
        if (bytesSinceGC > (10 * 1024 * 1024)) {
            bytesSinceGC = 0;
            gc();
        }
    }
    return num;
};

DBQuery.prototype.length = function() {
    return this.toArray().length;
};

DBQuery.prototype._addSpecial = function(name, value) {
    this._ensureSpecial();
    this._query[name] = value;
    return this;
};

DBQuery.prototype.sort = function(sortBy) {
    return this._addSpecial("orderby", sortBy);
};

DBQuery.prototype.hint = function(hint) {
    return this._addSpecial("$hint", hint);
};

DBQuery.prototype.min = function(min) {
    return this._addSpecial("$min", min);
};

DBQuery.prototype.max = function(max) {
    return this._addSpecial("$max", max);
};

/**
 * Deprecated. Use showRecordId().
 */
DBQuery.prototype.showDiskLoc = function() {
    return this.showRecordId();
};

DBQuery.prototype.showRecordId = function() {
    return this._addSpecial("$showDiskLoc", true);
};

DBQuery.prototype.maxTimeMS = function(maxTimeMS) {
    return this._addSpecial("$maxTimeMS", maxTimeMS);
};

DBQuery.prototype.readConcern = function(level) {
    var readConcernObj = {level: level};

    return this._addSpecial("readConcern", readConcernObj);
};

DBQuery.prototype.collation = function(collationSpec) {
    return this._addSpecial("collation", collationSpec);
};

/**
 * Sets the read preference for this cursor.
 *
 * @param mode {string} read preference mode to use.
 * @param tagSet {Array.<Object>} optional. The list of tags to use, order matters.
 *     Note that this object only keeps a shallow copy of this array.
 *
 * @return this cursor
 */
DBQuery.prototype.readPref = function(mode, tagSet) {
    var readPrefObj = {mode: mode};

    if (tagSet) {
        readPrefObj.tags = tagSet;
    }

    return this._addSpecial("$readPreference", readPrefObj);
};

DBQuery.prototype.forEach = function(func) {
    while (this.hasNext())
        func(this.next());
};

DBQuery.prototype.map = function(func) {
    var a = [];
    while (this.hasNext())
        a.push(func(this.next()));
    return a;
};

DBQuery.prototype.arrayAccess = function(idx) {
    return this.toArray()[idx];
};

DBQuery.prototype.comment = function(comment) {
    return this._addSpecial("$comment", comment);
};

DBQuery.prototype.explain = function(verbose) {
    var explainQuery = new DBExplainQuery(this, verbose);
    return explainQuery.finish();
};

DBQuery.prototype.snapshot = function() {
    return this._addSpecial("$snapshot", true);
};

DBQuery.prototype.returnKey = function() {
    return this._addSpecial("$returnKey", true);
};

DBQuery.prototype.maxScan = function(n) {
    return this._addSpecial("$maxScan", n);
};

DBQuery.prototype.pretty = function() {
    this._prettyShell = true;
    return this;
};

DBQuery.prototype.shellPrint = function() {
    try {
        var start = new Date().getTime();
        var n = 0;
        while (this.hasNext() && n < DBQuery.shellBatchSize) {
            var s = this._prettyShell ? tojson(this.next()) : tojson(this.next(), "", true);
            print(s);
            n++;
        }
        if (typeof _verboseShell !== 'undefined' && _verboseShell) {
            var time = new Date().getTime() - start;
            print("Fetched " + n + " record(s) in " + time + "ms");
        }
        if (this.hasNext()) {
            print("Type \"it\" for more");
            ___it___ = this;
        } else {
            ___it___ = null;
        }
    } catch (e) {
        print(e);
    }

};

/**
 * Returns a QueryPlan for the query.
 */
DBQuery.prototype.getQueryPlan = function() {
    return new QueryPlan(this);
};

DBQuery.prototype.toString = function() {
    return "DBQuery: " + this._ns + " -> " + tojson(this._query);
};

//
// CRUD specification find cursor extension
//

/**
* Get partial results from a mongos if some shards are down (instead of throwing an error).
*
* @method
* @see http://docs.mongodb.org/meta-driver/latest/legacy/mongodb-wire-protocol/#op-query
* @return {DBQuery}
*/
DBQuery.prototype.allowPartialResults = function() {
    this._checkModify();
    this.addOption(DBQuery.Option.partial);
    return this;
};

/**
* The server normally times out idle cursors after an inactivity period (10 minutes)
* to prevent excess memory use. Set this option to prevent that.
*
* @method
* @see http://docs.mongodb.org/meta-driver/latest/legacy/mongodb-wire-protocol/#op-query
* @return {DBQuery}
*/
DBQuery.prototype.noCursorTimeout = function() {
    this._checkModify();
    this.addOption(DBQuery.Option.noTimeout);
    return this;
};

/**
* Internal replication use only - driver should not set
*
* @method
* @see http://docs.mongodb.org/meta-driver/latest/legacy/mongodb-wire-protocol/#op-query
* @return {DBQuery}
*/
DBQuery.prototype.oplogReplay = function() {
    this._checkModify();
    this.addOption(DBQuery.Option.oplogReplay);
    return this;
};

/**
* Limits the fields to return for all matching documents.
*
* @method
* @see http://docs.mongodb.org/manual/tutorial/project-fields-from-query-results/
* @param {object} document Document specifying the projection of the resulting documents.
* @return {DBQuery}
*/
DBQuery.prototype.projection = function(document) {
    this._checkModify();
    this._fields = document;
    return this;
};

/**
* Specify cursor as a tailable cursor, allowing to specify if it will use awaitData
*
* @method
* @see http://docs.mongodb.org/manual/tutorial/create-tailable-cursor/
* @param {boolean} [awaitData=true] cursor blocks for a few seconds to wait for data if no documents
*found.
* @return {DBQuery}
*/
DBQuery.prototype.tailable = function(awaitData) {
    this._checkModify();
    this.addOption(DBQuery.Option.tailable);

    // Set await data if either specifically set or not specified
    if (awaitData || awaitData == null) {
        this.addOption(DBQuery.Option.awaitData);
    }

    return this;
};

/**
* Specify a document containing modifiers for the query.
*
* @method
* @see http://docs.mongodb.org/manual/reference/operator/query-modifier/
* @param {object} document A document containing modifers to apply to the cursor.
* @return {DBQuery}
*/
DBQuery.prototype.modifiers = function(document) {
    this._checkModify();

    for (var name in document) {
        if (name[0] != '$') {
            throw new Error('All modifiers must start with a $ such as $maxScan or $returnKey');
        }
    }

    for (var name in document) {
        this._addSpecial(name, document[name]);
    }

    return this;
};

DBQuery.prototype.close = function() {
    this._cursor.close();
};

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
    if (cmdResult.ok != 1) {
        throw _getErrorWithCode(cmdResult, "error: " + tojson(cmdResult));
    }

    this._batch = cmdResult.cursor.firstBatch.reverse();  // modifies input to allow popping

    if (mongo.useReadCommands()) {
        this._useReadCommands = true;
        this._cursorid = cmdResult.cursor.id;
        this._batchSize = batchSize;

        this._ns = cmdResult.cursor.ns;
        this._db = mongo.getDB(this._ns.substr(0, this._ns.indexOf(".")));
        this._collName = this._ns.substr(this._ns.indexOf(".") + 1);

        if (cmdResult.cursor.id) {
            // Note that setting this._cursorid to 0 should be accompanied by
            // this._cursorHandle.zeroCursorId().
            this._cursorHandle = mongo.cursorHandleFromId(cmdResult.cursor.ns, cmdResult.cursor.id);
        }
    } else {
        this._cursor = mongo.cursorFromId(cmdResult.cursor.ns, cmdResult.cursor.id, batchSize);
    }
}

DBCommandCursor.prototype = {};

DBCommandCursor.prototype.close = function() {
    if (!this._useReadCommands) {
        this._cursor.close();
    } else if (this._cursorid != 0) {
        var killCursorCmd = {
            killCursors: this._collName,
            cursors: [this._cursorid],
        };
        var cmdRes = this._db.runCommand(killCursorCmd);
        if (cmdRes.ok != 1) {
            throw _getErrorWithCode(cmdRes, "killCursors command failed: " + tojson(cmdRes));
        }

        this._cursorHandle.zeroCursorId();
        this._cursorid = NumberLong(0);
    }
};

/**
 * Fills out this._batch by running a getMore command. If the cursor is exhausted, also resets
 * this._cursorid to 0.
 *
 * Throws on error.
 */
DBCommandCursor.prototype._runGetMoreCommand = function() {
    // Construct the getMore command.
    var getMoreCmd = {getMore: this._cursorid, collection: this._collName};

    if (this._batchSize) {
        getMoreCmd["batchSize"] = this._batchSize;
    }

    // Deliver the getMore command, and check for errors in the response.
    var cmdRes = this._db.runCommand(getMoreCmd);
    if (cmdRes.ok != 1) {
        throw _getErrorWithCode(cmdRes, "getMore command failed: " + tojson(cmdRes));
    }

    if (this._ns !== cmdRes.cursor.ns) {
        throw Error("unexpected collection in getMore response: " + this._ns + " != " +
                    cmdRes.cursor.ns);
    }

    if (!cmdRes.cursor.id.compare(NumberLong("0"))) {
        this._cursorHandle.zeroCursorId();
        this._cursorid = NumberLong("0");
    } else if (this._cursorid.compare(cmdRes.cursor.id)) {
        throw Error("unexpected cursor id: " + this._cursorid.toString() + " != " +
                    cmdRes.cursor.id.toString());
    }

    // Successfully retrieved the next batch.
    this._batch = cmdRes.cursor.nextBatch.reverse();
};

DBCommandCursor.prototype._hasNextUsingCommands = function() {
    assert(this._useReadCommands);

    if (!this._batch.length) {
        if (!this._cursorid.compare(NumberLong("0"))) {
            return false;
        }

        this._runGetMoreCommand();
    }

    return this._batch.length > 0;
};

DBCommandCursor.prototype.hasNext = function() {
    if (this._useReadCommands) {
        return this._hasNextUsingCommands();
    }

    return this._batch.length || this._cursor.hasNext();
};

DBCommandCursor.prototype.next = function() {
    if (this._batch.length) {
        // $err wouldn't be in _firstBatch since ok was true.
        return this._batch.pop();
    } else if (this._useReadCommands) {
        // Have to call hasNext() here, as this is where we may issue a getMore in order to retrieve
        // the next batch of results.
        if (!this.hasNext())
            throw Error("error hasNext: false");
        return this._batch.pop();
    } else {
        if (!this._cursor.hasNext())
            throw Error("error hasNext: false");

        var ret = this._cursor.next();
        if (ret.$err)
            throw _getErrorWithCode(ret, "error: " + tojson(ret));
        return ret;
    }
};
DBCommandCursor.prototype.objsLeftInBatch = function() {
    if (this._useReadCommands) {
        return this._batch.length;
    } else if (this._batch.length) {
        return this._batch.length;
    } else {
        return this._cursor.objsLeftInBatch();
    }
};

DBCommandCursor.prototype.help = function() {
    // This is the same as the "Cursor Methods" section of DBQuery.help().
    print("\nCursor methods");
    print("\t.toArray() - iterates through docs and returns an array of the results");
    print("\t.forEach( func )");
    print("\t.map( func )");
    print("\t.hasNext()");
    print("\t.next()");
    print(
        "\t.objsLeftInBatch() - returns count of docs left in current batch (when exhausted, a new getMore will be issued)");
    print("\t.itcount() - iterates through documents and counts them");
    print("\t.pretty() - pretty print each document, possibly over multiple lines");
    print("\t.close()");
};

// Copy these methods from DBQuery
DBCommandCursor.prototype.toArray = DBQuery.prototype.toArray;
DBCommandCursor.prototype.forEach = DBQuery.prototype.forEach;
DBCommandCursor.prototype.map = DBQuery.prototype.map;
DBCommandCursor.prototype.itcount = DBQuery.prototype.itcount;
DBCommandCursor.prototype.shellPrint = DBQuery.prototype.shellPrint;
DBCommandCursor.prototype.pretty = DBQuery.prototype.pretty;

/**
 * QueryCache
 * Holds a reference to the cursor.
 * Proxy for planCache* query shape-specific commands.
 */
if ((typeof QueryPlan) == "undefined") {
    QueryPlan = function(cursor) {
        this._cursor = cursor;
    };
}

/**
 * Name of QueryPlan.
 * Same as collection.
 */
QueryPlan.prototype.getName = function() {
    return this._cursor._collection.getName();
};

/**
 * tojson prints the name of the collection
 */

QueryPlan.prototype.tojson = function(indent, nolint) {
    return tojson(this.getPlans());
};

/**
 * Displays help for a PlanCache object.
 */
QueryPlan.prototype.help = function() {
    var shortName = this.getName();
    print("QueryPlan help");
    print("\t.help() - show QueryPlan help");
    print("\t.clearPlans() - drops query shape from plan cache");
    print("\t.getPlans() - displays the cached plans for a query shape");
    return __magicNoPrint;
};

/**
 * List plans for a query shape.
 */
QueryPlan.prototype.getPlans = function() {
    return this._cursor._collection.getPlanCache().getPlansByQuery(this._cursor);
};

/**
 * Drop query shape from the plan cache.
 */
QueryPlan.prototype.clearPlans = function() {
    this._cursor._collection.getPlanCache().clearPlansByQuery(this._cursor);
    return;
};

const QueryHelpers = {
    _applyCountOptions: function _applyCountOptions(query, options) {
        const opts = Object.extend({}, options || {});

        if (typeof opts.skip == 'number') {
            query.skip(opts.skip);
        }

        if (typeof opts.limit == 'number') {
            query.limit(opts.limit);
        }

        if (typeof opts.maxTimeMS == 'number') {
            query.maxTimeMS(opts.maxTimeMS);
        }

        if (opts.hint) {
            query.hint(opts.hint);
        }

        if (typeof opts.readConcern == 'string') {
            query.readConcern(opts.readConcern);
        }

        if (typeof opts.collation == 'object') {
            query.collation(opts.collation);
        }
        return query;
    }
};
