// query.js

if (typeof DBQuery == "undefined") {
    DBQuery = function(
        mongo, db, collection, ns, filter, projection, limit, skip, batchSize, options) {
        this._mongo = mongo;
        this._db = db;
        this._collection = collection;
        this._ns = ns;

        this._filter = filter || {};
        this._projection = projection;
        this._limit = limit || 0;
        this._skip = skip || 0;
        this._batchSize = batchSize || 0;
        this._options = options || 0;

        // This houses find command parameters which are not passed to this constructor function or
        // held as properties of 'this'. When the find command represented by this 'DBQuery' is
        // assembled, this object will be appended to the find command object verbatim.
        this._additionalCmdParams = {};

        this._cursor = null;
        this._numReturned = 0;
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
    print("\t.maxTimeMS(<n>)");
    print("\t.comment(<comment>)");
    print("\t.tailable(<isAwaitData>)");
    print("\t.noCursorTimeout()");
    print("\t.allowPartialResults()");
    print("\t.returnKey()");
    print("\t.showRecordId() - adds a $recordId field to each returned object");
    print("\t.allowDiskUse() - allow using disk in completing the query");

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
    print("\t.getClusterTime() - returns the read timestamp for snapshot reads");
    print("\t.pretty() - pretty print each document, possibly over multiple lines");
};

DBQuery.prototype.clone = function() {
    const cloneResult = new DBQuery(this._mongo,
                                    this._db,
                                    this._collection,
                                    this._ns,
                                    this._filter,
                                    this._projection,
                                    this._limit,
                                    this._skip,
                                    this._batchSize,
                                    this._options);
    cloneResult._additionalCmdParams = this._additionalCmdParams;
    return cloneResult;
};

DBQuery.prototype._checkModify = function() {
    if (this._cursor)
        throw Error("query already executed");
};

DBQuery.prototype._isExhaustCursor = function() {
    return (this._options & DBQuery.Option.exhaust) !== 0;
};

/**
 * This method is exposed only for the purpose of testing and should not be used in most contexts.
 *
 * Indicates whether the OP_MSG moreToCome bit was set in the most recent getMore response received
 * for the cursor. Should always return false unless the 'exhaust' option was set when creating the
 * cursor.
 */
DBQuery.prototype._hasMoreToCome = function() {
    this._exec();

    if (this._cursor instanceof DBCommandCursor) {
        return false;
    }

    return this._cursor.hasMoreToCome();
};

DBQuery.prototype._exec = function() {
    if (!this._cursor) {
        assert.eq(0, this._numReturned);
        this._cursorSeen = 0;

        const findCmd = this._convertToCommand();

        // We forbid queries with the exhaust option from running as 'DBCommandCursor', because
        // 'DBCommandCursor' does not currently support exhaust.
        //
        // In the future, we could unify the shell's exhaust and non-exhaust code paths.
        if (!this._isExhaustCursor()) {
            const cmdRes = this._db.runReadCommand(findCmd, null, this._options);
            this._cursor = new DBCommandCursor(this._db, cmdRes, this._batchSize);
        } else {
            // The exhaust cursor option is disallowed under a session because it doesn't work as
            // expected, but all requests from the shell use implicit sessions, so to allow users to
            // continue using exhaust cursors through the shell, they are only disallowed with
            // explicit sessions.
            if (this._db.getSession()._isExplicit) {
                throw new Error("Explicit session is not allowed for exhaust queries");
            }

            if (findCmd["readConcern"]) {
                throw new Error("readConcern is not allowed for exhaust queries");
            }

            if (findCmd["collation"]) {
                throw new Error("collation is not allowed for exhaust queries");
            }

            if (findCmd["allowDiskUse"]) {
                throw new Error("allowDiskUse is not allowed for exhaust queries");
            }

            let readPreference = {};
            if (findCmd["$readPreference"]) {
                readPreference = findCmd["$readPreference"];
            }

            findCmd["$db"] = this._db.getName();
            this._cursor = this._mongo.find(findCmd, readPreference, true /*isExhaust*/);
        }
    }
    return this._cursor;
};

/**
 * Internal helper used to convert this cursor into the format required by the find command.
 */
DBQuery.prototype._convertToCommand = function() {
    let cmd = {};

    cmd["find"] = this._collection.getName();

    if (this._filter) {
        cmd["filter"] = this._filter;
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

    if (this._projection) {
        cmd["projection"] = this._projection;
    }

    if ((this._options & DBQuery.Option.tailable) != 0) {
        cmd["tailable"] = true;
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

    cmd = Object.merge(cmd, this._additionalCmdParams);

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
    return this._cursor.hasNext();
};

DBQuery.prototype.next = function() {
    this._exec();

    let o = this._cursor.hasNext();
    if (o)
        this._cursorSeen++;
    else
        throw Error("error hasNext: " + o);

    let ret = this._cursor.next();
    if (ret.$err) {
        throw _getErrorWithCode(ret, "error: " + tojson(ret));
    }

    this._numReturned++;
    return ret;
};

DBQuery.prototype.objsLeftInBatch = function() {
    this._exec();

    let ret = this._cursor.objsLeftInBatch();
    if (ret.$err)
        throw _getErrorWithCode(ret, "error: " + tojson(ret));

    return ret;
};

DBQuery.prototype.readOnly = function() {
    this._exec();
    this._cursor.readOnly();
    return this;
};

DBQuery.prototype.getId = function() {
    this._exec();
    return this._cursor.getId();
};

DBQuery.prototype.toArray = function() {
    if (this._arr)
        return this._arr;

    let a = [];
    while (this.hasNext())
        a.push(this.next());
    this._arr = a;
    return a;
};

DBQuery.prototype._convertToCountCmd = function(applySkipLimit) {
    let cmd = {count: this._collection.getName()};

    if (this._filter) {
        cmd["query"] = this._filter;
    }

    if (this._additionalCmdParams["maxTimeMS"]) {
        cmd["maxTimeMS"] = this._additionalCmdParams["maxTimeMS"];
    }
    if (this._additionalCmdParams["hint"]) {
        cmd["hint"] = this._additionalCmdParams["hint"];
    }
    if (this._additionalCmdParams["readConcern"]) {
        cmd["readConcern"] = this._additionalCmdParams["readConcern"];
    }
    if (this._additionalCmdParams["collation"]) {
        cmd["collation"] = this._additionalCmdParams["collation"];
    }

    if (applySkipLimit) {
        if (this._limit) {
            cmd["limit"] = this._limit;
        }
        if (this._skip) {
            cmd["skip"] = this._skip;
        }
    }

    return cmd;
};

DBQuery.prototype.count = function(applySkipLimit) {
    let cmd = this._convertToCountCmd(applySkipLimit);

    let res = this._db.runReadCommand(cmd);
    if (res && res.n != null)
        return res.n;
    throw _getErrorWithCode(res, "count failed: " + tojson(res));
};

DBQuery.prototype.size = function() {
    return this.count(true);
};

/**
 * iterative count - only for testing
 */
DBQuery.prototype.itcount = function() {
    let num = 0;

    // Track how many bytes we've used this cursor to iterate iterated.  This function can be called
    // with some very large cursors.  SpiderMonkey appears happy to allow these objects to
    // accumulate, so regular gc() avoids an overly large memory footprint.
    //
    // TODO: migrate this function into c++
    let bytesSinceGC = 0;

    while (this.hasNext()) {
        num++;
        let nextDoc = this.next();
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

DBQuery.prototype.sort = function(sortBy) {
    this._checkModify();
    this._additionalCmdParams["sort"] = sortBy;
    return this;
};

DBQuery.prototype.hint = function(hint) {
    this._checkModify();
    this._additionalCmdParams["hint"] = hint;
    return this;
};

DBQuery.prototype.min = function(min) {
    this._checkModify();
    this._additionalCmdParams["min"] = min;
    return this;
};

DBQuery.prototype.max = function(max) {
    this._checkModify();
    this._additionalCmdParams["max"] = max;
    return this;
};

/**
 * Deprecated. Use showRecordId().
 */
DBQuery.prototype.showDiskLoc = function() {
    return this.showRecordId();
};

DBQuery.prototype.showRecordId = function() {
    this._checkModify();
    this._additionalCmdParams["showRecordId"] = true;
    return this;
};

DBQuery.prototype.maxTimeMS = function(maxTimeMS) {
    this._checkModify();
    this._additionalCmdParams["maxTimeMS"] = maxTimeMS;
    return this;
};

DBQuery.prototype.readConcern = function(level, atClusterTime = undefined) {
    this._checkModify();
    let readConcernObj =
        atClusterTime ? {level: level, atClusterTime: atClusterTime} : {level: level};
    this._additionalCmdParams["readConcern"] = readConcernObj;
    return this;
};

DBQuery.prototype.collation = function(collationSpec) {
    this._checkModify();
    this._additionalCmdParams["collation"] = collationSpec;
    return this;
};

DBQuery.prototype.allowDiskUse = function(value) {
    this._checkModify();
    value = (value === undefined) ? true : value;
    this._additionalCmdParams["allowDiskUse"] = value;
    return this;
};

/**
 * Sets the read preference for this cursor.
 *
 * 'mode': A string indicating read preference mode to use.
 * 'tagSet': An optional list of tags to use. Order matters.
 * 'hedgeOptions': An optional object of the form {enabled: <bool>}.
 *
 * Returns 'this'.
 */
DBQuery.prototype.readPref = function(mode, tagSet, hedgeOptions) {
    let readPrefObj = {mode: mode};

    if (tagSet) {
        readPrefObj.tags = tagSet;
    }

    if (hedgeOptions) {
        readPrefObj.hedge = hedgeOptions;
    }

    this._additionalCmdParams["$readPreference"] = readPrefObj;
    return this;
};

DBQuery.prototype.forEach = function(func) {
    while (this.hasNext())
        func(this.next());
};

DBQuery.prototype.map = function(func) {
    let a = [];
    while (this.hasNext())
        a.push(func(this.next()));
    return a;
};

DBQuery.prototype.arrayAccess = function(idx) {
    return this.toArray()[idx];
};

DBQuery.prototype.comment = function(comment) {
    this._checkModify();
    this._additionalCmdParams["comment"] = comment;
    return this;
};

DBQuery.prototype.explain = function(verbose) {
    let explainQuery = new DBExplainQuery(this, verbose);
    return explainQuery.finish();
};

DBQuery.prototype.returnKey = function() {
    this._checkModify();
    this._additionalCmdParams["returnKey"] = true;
    return this;
};

DBQuery.prototype.pretty = function() {
    this._prettyShell = true;
    return this;
};

DBQuery.prototype.shellPrint = function() {
    try {
        let start = new Date().getTime();
        let n = 0;
        while (this.hasNext() && n < DBQuery.shellBatchSize) {
            let s = this._prettyShell ? tojson(this.next()) : tojson(this.next(), "", true);
            print(s);
            n++;
        }
        if (typeof _verboseShell !== 'undefined' && _verboseShell) {
            let time = new Date().getTime() - start;
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

DBQuery.prototype.toString = function() {
    return "DBQuery: " + this._ns + " -> " + tojson(this._filter);
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
 * Limits the fields to return for all matching documents.
 *
 * @method
 * @see http://docs.mongodb.org/manual/tutorial/project-fields-from-query-results/
 * @param {object} document Document specifying the projection of the resulting documents.
 * @return {DBQuery}
 */
DBQuery.prototype.projection = function(document) {
    this._checkModify();
    this._projection = document;
    return this;
};

/**
 * Specify cursor as a tailable cursor, allowing to specify if it will use awaitData
 *
 * @method
 * @see http://docs.mongodb.org/manual/tutorial/create-tailable-cursor/
 * @param {boolean} [awaitData=true] cursor blocks for a few seconds to wait for data if no
 *documents found.
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

DBQuery.prototype.close = function() {
    if (this._cursor) {
        this._cursor.close();
    }
};

DBQuery.prototype.isClosed = function() {
    this._exec();
    return this._cursor.isClosed();
};

DBQuery.prototype.isExhausted = function() {
    this._exec();
    return this._cursor.isClosed() && this._cursor.objsLeftInBatch() === 0;
};

DBQuery.prototype.getClusterTime = function() {
    // Return the read timestamp for snapshot reads, or undefined for other readConcern levels.
    this._exec();
    return this._cursor.getClusterTime();
};

DBQuery.shellBatchSize = 20;

/**
 * Query option flag bit constants.
 * @see http://dochub.mongodb.org/core/mongowireprotocol#MongoWireProtocol-OPQUERY
 */
DBQuery.Option = {
    tailable: 0x2,
    slaveOk: 0x4,
    // 0x8 is reserved for oplogReplay, but not explicitly defined. This is because the flag no
    // longer has any meaning to the server, and will be ignored, so there is no reason for it to
    // be set by clients.
    noTimeout: 0x10,
    awaitData: 0x20,
    exhaust: 0x40,
    partial: 0x80
};

function DBCommandCursor(db, cmdResult, batchSize, maxAwaitTimeMS, txnNumber) {
    if (cmdResult._mongo) {
        const newSession = new _DelegatingDriverSession(cmdResult._mongo, db.getSession());
        db = newSession.getDatabase(db.getName());
    }

    if (cmdResult.ok != 1) {
        throw _getErrorWithCode(cmdResult, "error: " + tojson(cmdResult));
    }

    this._batch = cmdResult.cursor.firstBatch.reverse();  // modifies input to allow popping

    // If the command result represents a snapshot read cursor, update our atClusterTime. And this
    // atClusterTime should not change over the lifetime of the cursor.
    if (cmdResult.cursor.atClusterTime) {
        this._atClusterTime = cmdResult.cursor.atClusterTime;
    }

    // If the command result represents a change stream cursor, update our postBatchResumeToken.
    this._updatePostBatchResumeToken(cmdResult.cursor);

    this._cursorid = cmdResult.cursor.id;
    this._batchSize = batchSize;
    this._maxAwaitTimeMS = maxAwaitTimeMS;
    this._txnNumber = txnNumber;

    this._ns = cmdResult.cursor.ns;
    this._db = db;
    this._collName = this._ns.substr(this._ns.indexOf(".") + 1);

    if (cmdResult.cursor.id) {
        // Note that setting this._cursorid to 0 should be accompanied by
        // this._cursorHandle.zeroCursorId().
        this._cursorHandle =
            this._db.getMongo().cursorHandleFromId(cmdResult.cursor.ns, cmdResult.cursor.id);
    }
}

DBCommandCursor.prototype = {};

/**
 * Returns whether the cursor id is zero.
 */
DBCommandCursor.prototype.isClosed = function() {
    return bsonWoCompare({_: this._cursorid}, {_: NumberLong(0)}) === 0;
};

/**
 * Returns whether the cursor has closed and has nothing in the batch.
 */
DBCommandCursor.prototype.isExhausted = function() {
    return this.isClosed() && this.objsLeftInBatch() === 0;
};

DBCommandCursor.prototype.close = function() {
    if (bsonWoCompare({_: this._cursorid}, {_: NumberLong(0)}) !== 0) {
        let killCursorCmd = {
            killCursors: this._collName,
            cursors: [this._cursorid],
        };
        let cmdRes = this._db.runCommand(killCursorCmd);
        if (cmdRes.ok != 1) {
            throw _getErrorWithCode(cmdRes, "killCursors command failed: " + tojson(cmdRes));
        }

        this._cursorHandle.zeroCursorId();
        this._cursorid = NumberLong(0);
    }
};

// Record the postBatchResumeToken from the given cursor object, if it exists. If the current batch
// is empty then this function also updates the current resume token to be the postBatchResumeToken.
DBCommandCursor.prototype._updatePostBatchResumeToken = function(cursorObj) {
    if (cursorObj.postBatchResumeToken) {
        this._postBatchResumeToken = cursorObj.postBatchResumeToken;
        if ((cursorObj.firstBatch || cursorObj.nextBatch).length === 0) {
            this._resumeToken = this._postBatchResumeToken;
        }
        this._isChangeStream = true;
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
    let getMoreCmd = {getMore: this._cursorid, collection: this._collName};

    if (this._batchSize) {
        getMoreCmd["batchSize"] = this._batchSize;
    }

    // maxAwaitTimeMS is only supported when using read commands.
    if (this._maxAwaitTimeMS) {
        getMoreCmd.maxTimeMS = this._maxAwaitTimeMS;
    }

    if (this._txnNumber) {
        getMoreCmd.txnNumber = NumberLong(this._txnNumber);
        getMoreCmd.autocommit = false;
    }

    // Deliver the getMore command, and check for errors in the response.
    let cmdRes = this._db.runCommand(getMoreCmd);
    assert.commandWorked(cmdRes, () => "getMore command failed: " + tojson(cmdRes));

    if (this._ns !== cmdRes.cursor.ns) {
        throw Error("unexpected collection in getMore response: " + this._ns +
                    " != " + cmdRes.cursor.ns);
    }

    if (!cmdRes.cursor.id.compare(NumberLong("0"))) {
        this._cursorHandle.zeroCursorId();
        this._cursorid = NumberLong("0");
    } else if (this._cursorid.compare(cmdRes.cursor.id)) {
        throw Error("unexpected cursor id: " + this._cursorid.toString() +
                    " != " + cmdRes.cursor.id.toString());
    }

    // If the command result represents a change stream cursor, update our postBatchResumeToken.
    this._updatePostBatchResumeToken(cmdRes.cursor);

    // Successfully retrieved the next batch.
    this._batch = cmdRes.cursor.nextBatch.reverse();

    // The read timestamp of a snapshot read cursor should not change over the lifetime of the
    // cursor.
    if (cmdRes.cursor.atClusterTime) {
        assert.eq(this._atClusterTime, cmdRes.cursor.atClusterTime);
    }
};

DBCommandCursor.prototype.hasNext = function() {
    if (!this._batch.length) {
        if (!this._cursorid.compare(NumberLong("0"))) {
            return false;
        }

        this._runGetMoreCommand();
    }

    return this._batch.length > 0;
};

DBCommandCursor.prototype.next = function() {
    if (this._batch.length) {
        // Pop the next result off the batch.
        const nextDoc = this._batch.pop();
        if (this._isChangeStream) {
            // If this is the last result in the batch, the postBatchResumeToken becomes the current
            // resume token for the cursor. Otherwise, the resume token is the _id of 'nextDoc'.
            this._resumeToken = (this._batch.length ? nextDoc._id : this._postBatchResumeToken);
        }
        return nextDoc;
    } else {
        // Have to call hasNext() here, as this is where we may issue a getMore in order to retrieve
        // the next batch of results.
        if (!this.hasNext())
            throw Error("error hasNext: false");
        return this._batch.pop();
    }
};
DBCommandCursor.prototype.objsLeftInBatch = function() {
    return this._batch.length;
};
DBCommandCursor.prototype.getId = function() {
    return this._cursorid;
};
DBCommandCursor.prototype.getResumeToken = function() {
    // Return the most recent recorded resume token, if such a token exists.
    return this._resumeToken;
};

DBCommandCursor.prototype.getClusterTime = function() {
    // Return the read timestamp for snapshot reads, or undefined for other readConcern levels.
    return this._atClusterTime;
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
    print(
        "\t.getResumeToken() - for a change stream cursor, obtains the most recent valid resume token, if it exists.");
    print("\t.getClusterTime() - returns the read timestamp for snapshot reads.");
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
