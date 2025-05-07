// @file collection.js - DBCollection support in the mongo shell
// db.colName is a DBCollection object
// or db["colName"]

if ((typeof DBCollection) == "undefined") {
    DBCollection = function(mongo, db, shortName, fullName) {
        this._mongo = mongo;
        this._db = db;
        this._shortName = shortName;
        this._fullName = fullName;
        this.verify();
    };
}

DBCollection.prototype.compact = function(extra = {}) {
    return this._db.getMongo().compact(this._fullName, extra);
};

DBCollection.prototype.cleanup = function(extra = {}) {
    return this._db.getMongo().cleanup(this._fullName, extra);
};

DBCollection.prototype._getCompactionTokens = function() {
    return this._db.getMongo()._getCompactionTokens(this._fullName);
};

DBCollection.prototype.verify = function() {
    assert(this._fullName, "no fullName");
    assert(this._shortName, "no shortName");
    assert(this._db, "no db");

    assert.eq(this._fullName, this._db._name + "." + this._shortName, "name mismatch");

    assert(this._mongo, "no mongo in DBCollection");
    assert(this.getMongo(), "no mongo from getMongo()");
};

DBCollection.prototype.getName = function() {
    return this._shortName;
};

DBCollection.prototype.help = function() {
    let shortName = this.getName();
    print("DBCollection help");
    print("\tdb." + shortName + ".find().help() - show DBCursor help");
    print(
        "\tdb." + shortName +
        ".bulkWrite( operations, <optional params> ) - bulk execute write operations, optional parameters are: w, wtimeout, j");
    print(
        "\tdb." + shortName +
        ".checkMetadataConsistency() - return metadata inconsistency information found in the collection.");
    print(
        "\tdb." + shortName +
        ".count( query = {}, <optional params> ) - count the number of documents that matches the query, optional parameters are: limit, skip, hint, maxTimeMS");
    print(
        "\tdb." + shortName +
        ".countDocuments( query = {}, <optional params> ) - count the number of documents that matches the query, optional parameters are: limit, skip, hint, maxTimeMS");
    print(
        "\tdb." + shortName +
        ".estimatedDocumentCount( <optional params> ) - estimate the document count using collection metadata, optional parameters are: maxTimeMS");
    print("\tdb." + shortName + ".convertToCapped(maxBytes) - calls {convertToCapped:'" +
          shortName + "', size:maxBytes}} command");
    print("\tdb." + shortName + ".createIndex(keypattern[,options])");
    print("\tdb." + shortName + ".createIndexes([keypatterns], <options>)");
    print("\tdb." + shortName + ".dataSize()");
    print(
        "\tdb." + shortName +
        ".deleteOne( filter, <optional params> ) - delete first matching document, optional parameters are: w, wtimeout, j");
    print(
        "\tdb." + shortName +
        ".deleteMany( filter, <optional params> ) - delete all matching documents, optional parameters are: w, wtimeout, j");
    print("\tdb." + shortName + ".distinct( key, query, <optional params> ) - e.g. db." +
          shortName + ".distinct( 'x' ), optional parameters are: maxTimeMS");
    print("\tdb." + shortName + ".drop() drop the collection");
    print("\tdb." + shortName + ".dropIndex(index) - e.g. db." + shortName +
          ".dropIndex( \"indexName\" ) or db." + shortName + ".dropIndex( { \"indexKey\" : 1 } )");
    print("\tdb." + shortName + ".hideIndex(index) - e.g. db." + shortName +
          ".hideIndex( \"indexName\" ) or db." + shortName + ".hideIndex( { \"indexKey\" : 1 } )");
    print("\tdb." + shortName + ".unhideIndex(index) - e.g. db." + shortName +
          ".unhideIndex( \"indexName\" ) or db." + shortName +
          ".unhideIndex( { \"indexKey\" : 1 } )");
    print("\tdb." + shortName + ".dropIndexes()");
    print("\tdb." + shortName + ".explain().help() - show explain help");
    print("\tdb." + shortName + ".reIndex()");
    print(
        "\tdb." + shortName +
        ".find([query],[fields]) - query is an optional query filter. fields is optional set of fields to return.");
    print("\t                                              e.g. db." + shortName +
          ".find( {x:77} , {name:1, x:1} )");
    print("\tdb." + shortName + ".find(...).count()");
    print("\tdb." + shortName + ".find(...).limit(n)");
    print("\tdb." + shortName + ".find(...).skip(n)");
    print("\tdb." + shortName + ".find(...).sort(...)");
    print("\tdb." + shortName + ".findOne([query], [fields], [options], [readConcern])");
    print(
        "\tdb." + shortName +
        ".findOneAndDelete( filter, <optional params> ) - delete first matching document, optional parameters are: projection, sort, maxTimeMS");
    print(
        "\tdb." + shortName +
        ".findOneAndReplace( filter, replacement, <optional params> ) - replace first matching document, optional parameters are: projection, sort, maxTimeMS, upsert, returnNewDocument");
    print(
        "\tdb." + shortName +
        ".findOneAndUpdate( filter, <update object or pipeline>, <optional params> ) - update first matching document, optional parameters are: projection, sort, maxTimeMS, upsert, returnNewDocument");
    print("\tdb." + shortName + ".getDB() get DB object associated with collection");
    print("\tdb." + shortName + ".getPlanCache() get query plan cache associated with collection");
    print("\tdb." + shortName + ".getIndexes()");
    print("\tdb." + shortName + ".insert(obj)");
    print(
        "\tdb." + shortName +
        ".insertOne( obj, <optional params> ) - insert a document, optional parameters are: w, wtimeout, j");
    print(
        "\tdb." + shortName +
        ".insertMany( [objects], <optional params> ) - insert multiple documents, optional parameters are: w, wtimeout, j");
    print("\tdb." + shortName + ".mapReduce( mapFunction , reduceFunction , <optional params> )");
    print(
        "\tdb." + shortName +
        ".aggregate( [pipeline], <optional params> ) - performs an aggregation on a collection; returns a cursor");
    print("\tdb." + shortName + ".remove(query)");
    print(
        "\tdb." + shortName +
        ".replaceOne( filter, replacement, <optional params> ) - replace the first matching document, optional parameters are: upsert, w, wtimeout, j");
    print("\tdb." + shortName +
          ".renameCollection( newName , <dropTarget> ) renames the collection.");
    print(
        "\tdb." + shortName +
        ".runCommand( name , <options> ) runs a db command with the given name where the first param is the collection name");
    print("\tdb." + shortName + ".save(obj)");
    print("\tdb." + shortName + ".stats({scale: N, indexDetails: true/false, " +
          "indexDetailsKey: <index key>, indexDetailsName: <index name>})");
    // print("\tdb." + shortName + ".diskStorageStats({[extent: <num>,] [granularity: <bytes>,]
    // ...}) - analyze record layout on disk");
    // print("\tdb." + shortName + ".pagesInRAM({[extent: <num>,] [granularity: <bytes>,] ...}) -
    // analyze resident memory pages");
    print("\tdb." + shortName +
          ".storageSize() - includes free space allocated to this collection");
    print("\tdb." + shortName + ".totalIndexSize() - size in bytes of all the indexes");
    print("\tdb." + shortName + ".totalSize() - storage allocated for all data and indexes");
    print(
        "\tdb." + shortName +
        ".update( query, <update object or pipeline>[, upsert_bool, multi_bool] ) - instead of two flags, you can pass an object with fields: upsert, multi, hint, let");
    print(
        "\tdb." + shortName +
        ".updateOne( filter, <update object or pipeline>, <optional params> ) - update the first matching document, optional parameters are: upsert, w, wtimeout, j, hint, let");
    print(
        "\tdb." + shortName +
        ".updateMany( filter, <update object or pipeline>, <optional params> ) - update all matching documents, optional parameters are: upsert, w, wtimeout, j, hint, let");
    print("\tdb." + shortName + ".validate( <full> ) - SLOW");
    print("\tdb." + shortName + ".getShardVersion() - only for use with sharding");
    print("\tdb." + shortName +
          ".getShardDistribution() - prints statistics about data distribution in the cluster");
    print("\tdb." + shortName + ".getShardKey() - prints the shard key for this collection");
    print(
        "\tdb." + shortName +
        ".getSplitKeysForChunks( <maxChunkSize> ) - calculates split points over all chunks and returns splitter function");
    print("\tdb." + shortName + ".disableBalancing() - disables the balancer for this collection");
    print("\tdb." + shortName + ".enableBalancing() - enables the balancer for this collection");
    print(
        "\tdb." + shortName +
        ".getWriteConcern() - returns the write concern used for any operations on this collection, inherited from server/db if set");
    print(
        "\tdb." + shortName +
        ".setWriteConcern( <write concern doc> ) - sets the write concern for writes to the collection");
    print(
        "\tdb." + shortName +
        ".unsetWriteConcern( <write concern doc> ) - unsets the write concern for writes to the collection");
    print("\tdb." + shortName +
          ".latencyStats() - display operation latency histograms for this collection");
    print("\tdb." + shortName + ".disableAutoMerger() - disable auto-merging on this collection");
    print("\tdb." + shortName + ".enableAutoMerger() - enable auto-merge on this collection");
    return __magicNoPrint;
};

DBCollection.prototype.getFullName = function() {
    return this._fullName;
};
DBCollection.prototype.getMongo = function() {
    return this._db.getMongo();
};
DBCollection.prototype.getDB = function() {
    return this._db;
};

DBCollection.prototype._makeCommand = function(cmd, params) {
    let c = {};
    c[cmd] = this.getName();
    if (params)
        Object.extend(c, params);
    return c;
};

DBCollection.prototype._dbCommand = function(cmd, params) {
    if (typeof (cmd) === "object")
        return this._db._dbCommand(cmd, {}, this.getQueryOptions());

    return this._db._dbCommand(this._makeCommand(cmd, params), {}, this.getQueryOptions());
};

// Like _dbCommand, but applies $readPreference
DBCollection.prototype._dbReadCommand = function(cmd, params) {
    if (typeof (cmd) === "object")
        return this._db._dbReadCommand(cmd, {}, this.getQueryOptions());

    return this._db._dbReadCommand(this._makeCommand(cmd, params), {}, this.getQueryOptions());
};

DBCollection.prototype.runCommand = DBCollection.prototype._dbCommand;

DBCollection.prototype.runReadCommand = DBCollection.prototype._dbReadCommand;

DBCollection.prototype._massageObject = function(q) {
    if (!q)
        return {};

    let type = typeof q;

    if (type == "function")
        return {$where: q};

    if (q.isObjectId)
        return {_id: q};

    if (type == "object")
        return q;

    if (type == "string") {
        // If the string is 24 hex characters, it is most likely an ObjectId.
        if (/^[0-9a-fA-F]{24}$/.test(q)) {
            return {_id: ObjectId(q)};
        }

        return {$where: q};
    }

    throw Error("don't know how to massage : " + type);
};

DBCollection.prototype.find = function(filter, projection, limit, skip, batchSize, options) {
    // Verify that API version parameters are not supplied via the shell helper.
    assert.noAPIParams(options);

    let cursor = new DBQuery(this._mongo,
                             this._db,
                             this,
                             this._fullName,
                             this._massageObject(filter),
                             projection,
                             limit,
                             skip,
                             batchSize,
                             options || this.getQueryOptions());

    {
        const session = this.getDB().getSession();

        const readPreference = session._getSessionAwareClient().getReadPreference(session);
        if (readPreference !== null) {
            cursor.readPref(readPreference.mode, readPreference.tags);
        }

        const client = session._getSessionAwareClient();
        const readConcern = client.getReadConcern(session);
        if (readConcern !== null && client.canUseReadConcern(session, cursor._convertToCommand())) {
            cursor.readConcern(readConcern.level);
        }
    }

    return cursor;
};

DBCollection.prototype.findOne = function(
    filter, projection, options, readConcern, collation, rawData) {
    let cursor =
        this.find(filter, projection, -1 /* limit */, 0 /* skip*/, 0 /* batchSize */, options);

    if (readConcern) {
        cursor = cursor.readConcern(readConcern);
    }

    if (collation) {
        cursor = cursor.collation(collation);
    }

    if (rawData) {
        cursor = cursor.rawData();
    }

    if (!cursor.hasNext())
        return null;
    let ret = cursor.next();
    if (cursor.hasNext())
        throw Error("findOne has more than 1 result!");
    if (ret.$err)
        throw _getErrorWithCode(ret, "error " + tojson(ret));
    return ret;
};

// Returns a WriteResult for a single insert or a BulkWriteResult for a multi-insert if write
// command succeeded, but may contain write errors.
// Returns a WriteCommandError if the write command responded with ok:0.
DBCollection.prototype.insert = function(obj, options) {
    if (!obj)
        throw Error("no object passed to insert!");

    options = typeof (options) === 'undefined' ? {} : options;

    let flags = 0;

    let wc = undefined;
    let rawData = undefined;
    if (options === undefined) {
        // do nothing
    } else if (typeof (options) == 'object') {
        if (options.ordered === undefined) {
            // do nothing, like above
        } else {
            flags = options.ordered ? 0 : 1;
        }

        if (options.writeConcern)
            wc = options.writeConcern;

        if (options.rawData)
            rawData = options.rawData;
    } else {
        flags = options;
    }

    // 1 = continueOnError, which is synonymous with unordered in the write commands/bulk-api
    let ordered = ((flags & 1) == 0);

    if (!wc)
        wc = this._createWriteConcern(options);

    let result = undefined;

    // Bit 1 of option flag is continueOnError. Bit 0 (stop on error) is the default.
    let bulk = ordered ? this.initializeOrderedBulkOp() : this.initializeUnorderedBulkOp();
    if (rawData)
        bulk.setRawData(rawData);
    let isMultiInsert = Array.isArray(obj);

    if (isMultiInsert) {
        obj.forEach(function(doc) {
            bulk.insert(doc);
        });
    } else {
        bulk.insert(obj);
    }

    try {
        result = bulk.execute(wc);
        if (!isMultiInsert)
            result = result.toSingleResult();
    } catch (ex) {
        if (ex instanceof BulkWriteError) {
            result = isMultiInsert ? ex.toResult() : ex.toSingleResult();
        } else if (ex instanceof WriteCommandError) {
            result = ex;
        } else {
            // Other exceptions rethrown as-is.
            throw ex;
        }
    }
    return result;
};

/**
 * Does validation of the remove args. Throws if the parse is not successful, otherwise
 * returns a document {query: <query>, justOne: <limit>, wc: <writeConcern>}.
 */
DBCollection.prototype._parseRemove = function(t, justOne) {
    if (undefined === t)
        throw Error("remove needs a query");

    let query = this._massageObject(t);

    let wc = undefined;
    let collation = undefined;
    let letParams = undefined;
    let rawData = undefined;

    if (typeof (justOne) === "object") {
        let opts = justOne;
        wc = opts.writeConcern;
        justOne = opts.justOne;
        collation = opts.collation;
        letParams = opts.let;
        rawData = opts.rawData;
    }

    // Normalize "justOne" to a bool.
    justOne = justOne ? true : false;

    // Handle write concern.
    if (!wc) {
        wc = this.getWriteConcern();
    }

    return {
        "query": query,
        "justOne": justOne,
        "wc": wc,
        "collation": collation,
        "let": letParams,
        "rawData": rawData,
    };
};

// Returns a WriteResult if write command succeeded, but may contain write errors.
// Returns a WriteCommandError if the write command responded with ok:0.
DBCollection.prototype.remove = function(t, justOne) {
    let parsed = this._parseRemove(t, justOne);
    let query = parsed.query;
    justOne = parsed.justOne;
    let wc = parsed.wc;
    let collation = parsed.collation;
    let letParams = parsed.let;
    let rawData = parsed.rawData;

    let result = undefined;
    let bulk = this.initializeOrderedBulkOp();

    if (letParams) {
        bulk.setLetParams(letParams);
    }

    if (rawData) {
        bulk.setRawData(rawData);
    }

    let removeOp = bulk.find(query);

    if (collation) {
        removeOp.collation(collation);
    }

    if (justOne) {
        removeOp.removeOne();
    } else {
        removeOp.remove();
    }

    try {
        result = bulk.execute(wc).toSingleResult();
    } catch (ex) {
        if (ex instanceof BulkWriteError) {
            result = ex.toSingleResult();
        } else if (ex instanceof WriteCommandError) {
            result = ex;
        } else {
            // Other exceptions thrown
            throw ex;
        }
    }
    return result;
};

/**
 * Does validation of the update args. Throws if the parse is not successful, otherwise returns a
 * document containing fields for query, updateSpec, upsert, multi, wc, collation, and arrayFilters.
 *
 * Throws if the arguments are invalid.
 */
DBCollection.prototype._parseUpdate = function(query, updateSpec, upsert, multi) {
    if (!query)
        throw Error("need a query");
    if (!updateSpec)
        throw Error("need an update object or pipeline");

    let wc = undefined;
    let collation = undefined;
    let arrayFilters = undefined;
    let hint = undefined;
    let letParams = undefined;
    let rawData = undefined;

    // can pass options via object for improved readability
    if (typeof (upsert) === "object") {
        if (multi) {
            throw Error("Fourth argument must be empty when specifying " +
                        "upsert and multi with an object.");
        }

        let opts = upsert;
        multi = opts.multi;
        wc = opts.writeConcern;
        upsert = opts.upsert;
        collation = opts.collation;
        arrayFilters = opts.arrayFilters;
        hint = opts.hint;
        letParams = opts.let;
        rawData = opts.rawData;
        if (opts.sort) {
            throw new Error(
                "This sort will not do anything. Please call update without a sort or defer to calling updateOne with a sort.");
        }
    }

    // Normalize 'upsert' and 'multi' to booleans.
    upsert = upsert ? true : false;
    multi = multi ? true : false;

    if (!wc) {
        wc = this.getWriteConcern();
    }

    return {
        "query": query,
        "updateSpec": updateSpec,
        "hint": hint,
        "upsert": upsert,
        "multi": multi,
        "wc": wc,
        "collation": collation,
        "arrayFilters": arrayFilters,
        "let": letParams,
        "rawData": rawData,
    };
};

// Returns a WriteResult if write command succeeded, but may contain write errors.
// Returns a WriteCommandError if the write command responded with ok:0.
DBCollection.prototype.update = function(query, updateSpec, upsert, multi) {
    let parsed = this._parseUpdate(query, updateSpec, upsert, multi);
    query = parsed.query;
    updateSpec = parsed.updateSpec;
    const hint = parsed.hint;
    upsert = parsed.upsert;
    multi = parsed.multi;
    let wc = parsed.wc;
    let collation = parsed.collation;
    let arrayFilters = parsed.arrayFilters;
    let letParams = parsed.let;
    let rawData = parsed.rawData;

    let result = undefined;
    let bulk = this.initializeOrderedBulkOp();

    if (letParams) {
        bulk.setLetParams(letParams);
    }

    if (rawData) {
        bulk.setRawData(rawData);
    }

    let updateOp = bulk.find(query);

    if (hint) {
        updateOp.hint(hint);
    }

    if (upsert) {
        updateOp = updateOp.upsert();
    }

    if (collation) {
        updateOp.collation(collation);
    }

    if (arrayFilters) {
        updateOp.arrayFilters(arrayFilters);
    }

    if (rawData) {
        bulk.setRawData(rawData);
    }

    if (multi) {
        updateOp.update(updateSpec);
    } else {
        updateOp.updateOne(updateSpec);
    }

    try {
        result = bulk.execute(wc).toSingleResult();
    } catch (ex) {
        if (ex instanceof BulkWriteError) {
            result = ex.toSingleResult();
        } else if (ex instanceof WriteCommandError) {
            result = ex;
        } else {
            // Other exceptions thrown
            throw ex;
        }
    }
    return result;
};

DBCollection.prototype.save = function(obj, opts) {
    if (obj == null)
        throw Error("can't save a null");

    if (typeof (obj) == "number" || typeof (obj) == "string" || Array.isArray(obj))
        throw Error("can't save a number, a string or an array");

    if (typeof (obj._id) == "undefined") {
        obj._id = new ObjectId();
        return this.insert(obj, opts);
    } else {
        return this.update({_id: obj._id}, obj, Object.merge({upsert: true}, opts));
    }
};

DBCollection.prototype._genIndexName = function(keys) {
    let name = "";
    for (let k in keys) {
        let v = keys[k];
        if (typeof v == "function")
            continue;

        if (name.length > 0)
            name += "_";
        name += k + "_";

        name += v;
    }
    return name;
};

DBCollection.prototype._indexSpec = function(keys, options) {
    let ret = {ns: this._fullName, key: keys, name: this._genIndexName(keys)};

    if (!options) {
    } else if (typeof (options) == "string")
        ret.name = options;
    else if (typeof (options) == "boolean")
        ret.unique = true;
    else if (typeof (options) == "object") {
        if (Array.isArray(options)) {
            if (options.length > 3) {
                throw new Error("Index options that are supplied in array form may only specify" +
                                " three values: name, unique, dropDups");
            }
            let nb = 0;
            for (let i = 0; i < options.length; i++) {
                if (typeof (options[i]) == "string")
                    ret.name = options[i];
                else if (typeof (options[i]) == "boolean") {
                    if (options[i]) {
                        if (nb == 0)
                            ret.unique = true;
                        if (nb == 1)
                            ret.dropDups = true;
                    }
                    nb++;
                }
            }
        } else {
            Object.extend(ret, options);
        }
    } else {
        throw Error("can't handle: " + typeof (options));
    }

    return ret;
};

DBCollection.prototype.createIndex = function(keys, options, commitQuorum) {
    if (arguments.length > 3) {
        throw new Error("createIndex accepts up to 3 arguments");
    }

    return this.createIndexes([keys], options, commitQuorum);
};

DBCollection.prototype.createIndexes = function(keys, options, commitQuorum) {
    if (arguments.length > 3) {
        throw new Error("createIndexes accepts up to 3 arguments");
    }

    if (!Array.isArray(keys)) {
        throw new Error("createIndexes first argument should be an array");
    }

    let indexSpecs = Array(keys.length);
    for (let i = 0; i < indexSpecs.length; i++) {
        indexSpecs[i] = this._indexSpec(keys[i], options);
    }

    for (let i = 0; i < indexSpecs.length; i++) {
        delete (indexSpecs[i].ns);  // ns is passed to the first element in the command.
    }

    if (commitQuorum === undefined) {
        return this._db.runCommand({createIndexes: this.getName(), indexes: indexSpecs});
    }
    return this._db.runCommand(
        {createIndexes: this.getName(), indexes: indexSpecs, commitQuorum: commitQuorum});
};

DBCollection.prototype.reIndex = function() {
    return this._db.runCommand({reIndex: this.getName()});
};

DBCollection.prototype.dropIndexes = function(indexNames) {
    indexNames = indexNames || '*';
    let res = this._db.runCommand({dropIndexes: this.getName(), index: indexNames});
    assert(res, "no result from dropIndex result");
    if (res.ok)
        return res;

    if (res.errmsg.match(/not found/))
        return res;

    throw _getErrorWithCode(res, "error dropping indexes : " + tojson(res));
};

DBCollection.prototype.drop = function(options = {}) {
    const cmdObj = Object.assign({drop: this.getName()}, options);
    const ret = this._db.runCommand(cmdObj);
    if (!ret.ok) {
        if (ret.errmsg == "ns not found")
            return false;
        throw _getErrorWithCode(ret, "drop failed: " + tojson(ret));
    }
    return true;
};

DBCollection.prototype.findAndModify = function(args) {
    let cmd = {findandmodify: this.getName()};
    for (let key in args) {
        cmd[key] = args[key];
    }

    {
        const kWireVersionSupportingRetryableWrites = 6;
        const serverSupportsRetryableWrites =
            this.getMongo().getMinWireVersion() <= kWireVersionSupportingRetryableWrites &&
            kWireVersionSupportingRetryableWrites <= this.getMongo().getMaxWireVersion();

        const session = this.getDB().getSession();
        if (serverSupportsRetryableWrites && session.getOptions().shouldRetryWrites() &&
            _ServerSession.canRetryWrites(cmd)) {
            cmd = session._serverSession.assignTransactionNumber(cmd);
        }
    }

    let ret = this._db.runCommand(cmd);
    if (!ret.ok) {
        if (ret.errmsg == "No matching object found") {
            return null;
        }
        throw _getErrorWithCode(ret, "findAndModifyFailed failed: " + tojson(ret));
    }
    return ret.value;
};

DBCollection.prototype.renameCollection = function(newName, dropTarget) {
    if (arguments.length === 1 && typeof newName === 'object') {
        if (newName.hasOwnProperty('dropTarget')) {
            dropTarget = newName['dropTarget'];
        }
        newName = newName['to'];
    }
    if (typeof dropTarget === 'undefined') {
        dropTarget = false;
    }
    if (typeof newName !== 'string' || typeof dropTarget !== 'boolean') {
        throw Error(
            'renameCollection must either take a string and an optional boolean or an object.');
    }
    return this._db._adminCommand({
        renameCollection: this._fullName,
        to: this._db._name + "." + newName,
        dropTarget: dropTarget
    });
};

DBCollection.prototype.validate = function(options) {
    if (typeof (options) != 'object' && typeof (options) != 'undefined') {
        return "expected optional options to be of the following format: {full: <bool>, background: <bool>}.";
    }

    let cmd = {validate: this.getName()};
    Object.assign(cmd, options || {});

    let res = this._db.runCommand(cmd);

    if (typeof (res.valid) == 'undefined') {
        // old-style format just put everything in a string. Now using proper fields

        res.valid = false;

        let raw = res.result || res.raw;

        if (raw) {
            let str = "-" + tojson(raw);
            res.valid = !(str.match(/exception/) || str.match(/corrupt/));

            let p = /lastExtentSize:(\d+)/;
            let r = p.exec(str);
            if (r) {
                res.lastExtentSize = Number(r[1]);
            }
        }
    }

    return res;
};

DBCollection.prototype.getShardVersion = function() {
    return this._db._adminCommand({getShardVersion: this._fullName});
};

DBCollection.prototype.getIndexes = function() {
    let res = this.runCommand("listIndexes");

    if (!res.ok) {
        if (res.code == ErrorCodes.NamespaceNotFound) {
            // For compatibility, return []
            return [];
        }

        throw _getErrorWithCode(res, "listIndexes failed: " + tojson(res));
    }

    return new DBCommandCursor(this._db, res).toArray();
};

DBCollection.prototype.getIndices = DBCollection.prototype.getIndexes;
DBCollection.prototype.getIndexSpecs = DBCollection.prototype.getIndexes;

DBCollection.prototype.getIndexKeys = function() {
    return this.getIndexes().map(function(i) {
        return i.key;
    });
};

DBCollection.prototype.hashAllDocs = function() {
    let cmd = {dbhash: 1, collections: [this._shortName]};
    let res = this._dbCommand(cmd);
    let hash = res.collections[this._shortName];
    assert(hash);
    assert(typeof (hash) == "string");
    return hash;
};

/**
 * Drop a specified index.
 *
 * "index" is the name or key pattern of the index. For example:
 *    db.collectionName.dropIndex( "myIndexName" );
 *    db.collectionName.dropIndex( { "indexKey" : 1 } );
 *
 * @param {String} name or key object of index to delete.
 * @return A result object.  result.ok will be true if successful.
 */
DBCollection.prototype.dropIndex = function(index) {
    assert(index, "need to specify index to dropIndex");

    // Need an extra check for array because 'Array' is an 'object', but not every 'object' is an
    // 'Array'.
    if (typeof index != "string" && typeof index != "object" || index instanceof Array) {
        throw new Error(
            "The index to drop must be either the index name or the index specification document");
    }

    if (typeof index == "string" && index === "*") {
        throw new Error(
            "To drop indexes in the collection using '*', use db.collection.dropIndexes()");
    }

    let res = this._dbCommand("dropIndexes", {index: index});
    return res;
};

/**
 * Hide an index from the query planner.
 */
DBCollection.prototype._hiddenIndex = function(index, hidden) {
    assert(index, "please specify index to hide");

    // Need an extra check for array because 'Array' is an 'object', but not every 'object' is an
    // 'Array'.
    let indexField = {};
    if (typeof index == "string") {
        indexField = {name: index, hidden: hidden};
    } else if (typeof index == "object") {
        indexField = {keyPattern: index, hidden: hidden};
    } else {
        throw new Error("Index must be either the index name or the index specification document");
    }
    let cmd = {"collMod": this._shortName, index: indexField};
    let res = this._db.runCommand(cmd);
    return res;
};

DBCollection.prototype.hideIndex = function(index) {
    return this._hiddenIndex(index, true);
};

DBCollection.prototype.unhideIndex = function(index) {
    return this._hiddenIndex(index, false);
};

DBCollection.prototype.getCollection = function(subName) {
    return this._db.getCollection(this._shortName + "." + subName);
};

/**
 * scale: The scale at which to deliver results. Unless specified, this command returns all data
 *        in bytes.
 * indexDetails: Includes indexDetails field in results. Default: false.
 * indexDetailsKey: If indexDetails is true, filter contents in indexDetails by this index key.
 * indexDetailsname: If indexDetails is true, filter contents in indexDetails by this index name.
 *
 * It is an error to provide both indexDetailsKey and indexDetailsName.
 */
DBCollection.prototype.stats = function(args) {
    'use strict';

    // For backwards compatibility with db.collection.stats(scale).
    let scale = isObject(args) ? args.scale : args;

    let options = isObject(args) ? args : {};
    if (options.indexDetailsKey && options.indexDetailsName) {
        throw new Error('Cannot filter indexDetails on both indexDetailsKey and ' +
                        'indexDetailsName');
    }
    // collStats can run on a secondary, so we need to apply readPreference
    let res = this._db.runReadCommand({collStats: this._shortName, scale: scale});
    if (!res.ok) {
        return res;
    }

    let getIndexName = function(collection, indexKey) {
        if (!isObject(indexKey))
            return undefined;
        let indexName;
        collection.getIndexes().forEach(function(spec) {
            if (friendlyEqual(spec.key, options.indexDetailsKey)) {
                indexName = spec.name;
            }
        });
        return indexName;
    };

    let filterIndexName = options.indexDetailsName || getIndexName(this, options.indexDetailsKey);

    let updateStats = function(stats, keepIndexDetails, indexName) {
        if (!stats.indexDetails)
            return;
        if (!keepIndexDetails) {
            delete stats.indexDetails;
            return;
        }
        if (!indexName)
            return;
        for (let key in stats.indexDetails) {
            if (key == indexName)
                continue;
            delete stats.indexDetails[key];
        }
    };

    updateStats(res, options.indexDetails, filterIndexName);

    if (res.sharded) {
        for (let shardName in res.shards) {
            updateStats(res.shards[shardName], options.indexDetails, filterIndexName);
        }
    }

    return res;
};

DBCollection.prototype.dataSize = function() {
    return this.stats().size;
};

DBCollection.prototype.storageSize = function() {
    return this.stats().storageSize;
};

DBCollection.prototype.totalIndexSize = function(verbose) {
    let stats = this.stats();
    if (verbose) {
        for (let ns in stats.indexSizes) {
            print(ns + "\t" + stats.indexSizes[ns]);
        }
    }
    return stats.totalIndexSize;
};

DBCollection.prototype.totalSize = function() {
    let total = this.storageSize();
    let totalIndexSize = this.totalIndexSize();
    if (totalIndexSize) {
        total += totalIndexSize;
    }
    return total;
};

DBCollection.prototype.convertToCapped = function(bytes) {
    if (!bytes)
        throw Error("have to specify # of bytes");
    return this._dbCommand({convertToCapped: this._shortName, size: bytes});
};

/*
 * Returns metadata for the collection using listCollections.
 *
 * If the collection does not exists return null.
 */
DBCollection.prototype.getMetadata = function() {
    let res = this._db.runCommand("listCollections", {filter: {name: this._shortName}});
    if (res.ok) {
        const cursor = new DBCommandCursor(this._db, res);
        if (!cursor.hasNext())
            return null;
        return cursor.next();
    }

    throw _getErrorWithCode(res, "listCollections failed: " + tojson(res));
};

DBCollection.prototype.exists = function() {
    return this.getMetadata();
};

DBCollection.prototype.isCapped = function() {
    const m = this.getMetadata();
    return (m && m.options && m.options.capped) ? true : false;
};

DBCollection.prototype.getUUID = function() {
    const m = this.getMetadata();
    if (!m) {
        throw Error(`Collection '${this}' does not exist.`);
    }
    return m.info.uuid;
};

//
// CRUD specification aggregation cursor extension
//
DBCollection.prototype.aggregate = function(pipeline, aggregateOptions) {
    if (!(pipeline instanceof Array)) {
        // Support legacy varargs form. Also handles db.foo.aggregate().
        pipeline = Array.from(arguments);
        aggregateOptions = {};
    } else if (aggregateOptions === undefined) {
        aggregateOptions = {};
    }

    const cmdObj = this._makeCommand("aggregate", {pipeline: pipeline});

    return this._db._runAggregate(cmdObj, aggregateOptions);
};

DBCollection.prototype.convertToSingleObject = function(valueField) {
    let z = {};
    this.find().forEach(function(a) {
        z[a._id] = a[valueField];
    });
    return z;
};

/**
 * @param optional object of optional fields;
 */
DBCollection.prototype.mapReduce = function(map, reduce, optionsOrOutString) {
    let c = {mapreduce: this._shortName, map: map, reduce: reduce};
    assert(optionsOrOutString, "need to supply an optionsOrOutString");

    if (typeof (optionsOrOutString) == "string")
        c["out"] = optionsOrOutString;
    else
        Object.extend(c, optionsOrOutString);

    let output;

    if (c["out"].hasOwnProperty("inline") && c["out"]["inline"] === 1) {
        // if inline output is specified, we need to apply readPreference on the command
        // as it could be run on a secondary
        output = this._db.runReadCommand(c);
    } else {
        output = this._db.runCommand(c);
    }

    if (!output.ok) {
        __mrerror__ = output;  // eslint-disable-line
        throw _getErrorWithCode(output, "map reduce failed:" + tojson(output));
    }
    return output;
};

DBCollection.prototype.toString = function() {
    return this.getFullName();
};

DBCollection.prototype.tojson = DBCollection.prototype.toString;

DBCollection.prototype.shellPrint = DBCollection.prototype.toString;

DBCollection.autocomplete = function(obj) {
    let colls = DB.autocomplete(obj.getDB());
    let ret = [];
    for (let i = 0; i < colls.length; i++) {
        let c = colls[i];
        if (c.length <= obj.getName().length)
            continue;
        if (c.slice(0, obj.getName().length + 1) != obj.getName() + '.')
            continue;

        ret.push(c.slice(obj.getName().length + 1));
    }
    return ret;
};

/**
 * Return true if the collection has been sharded.
 *
 * @method
 * @return {boolean}
 */
DBCollection.prototype._isSharded = function() {
    // Checking for 'dropped: {$ne: true}' to ensure mongo shell compatibility with earlier versions
    // of the server
    return !!this._db.getSiblingDB("config").collections.countDocuments(
        {_id: this._fullName, dropped: {$ne: true}});
};

/**
 * Prints statistics related to the collection's data distribution
 */
DBCollection.prototype.getShardDistribution = function() {
    let config = this.getDB().getSiblingDB("config");

    if (!this._isSharded()) {
        print("Collection " + this + " is not sharded.");
        return;
    }

    let collStats = this.aggregate({"$collStats": {storageStats: {}}});

    let totals = {numChunks: 0, size: 0, count: 0};
    let conciseShardsStats = [];

    collStats.forEach(function(extShardStats) {
        // Extract and store only the relevant subset of the stats for this shard
        let newVersion = config.collections.countDocuments(
            {_id: extShardStats.ns, timestamp: {$exists: true}}, {limit: 1});
        let collUuid = config.collections.findOne({_id: extShardStats.ns}).uuid;
        const shardStats = {
            shardId: extShardStats.shard,
            host: config.shards.findOne({_id: extShardStats.shard}).host,
            size: extShardStats.storageStats.size,
            count: extShardStats.storageStats.count,
            numChunks: (newVersion ? config.chunks.countDocuments(
                                         {uuid: collUuid, shard: extShardStats.shard})
                                   : config.chunks.countDocuments(
                                         {ns: extShardStats.ns, shard: extShardStats.shard})),
            avgObjSize: extShardStats.storageStats.avgObjSize
        };

        print("\nShard " + shardStats.shardId + " at " + shardStats.host);

        let estChunkData =
            (shardStats.numChunks == 0) ? 0 : (shardStats.size / shardStats.numChunks);
        let estChunkCount =
            (shardStats.numChunks == 0) ? 0 : Math.floor(shardStats.count / shardStats.numChunks);
        print(" data : " + sh._dataFormat(shardStats.size) + " docs : " + shardStats.count +
              " chunks : " + shardStats.numChunks);
        print(" estimated data per chunk : " + sh._dataFormat(estChunkData));
        print(" estimated docs per chunk : " + estChunkCount);

        totals.size += shardStats.size;
        totals.count += shardStats.count;
        totals.numChunks += shardStats.numChunks;

        conciseShardsStats.push(shardStats);
    });

    print("\nTotals");
    print(" data : " + sh._dataFormat(totals.size) + " docs : " + totals.count +
          " chunks : " + totals.numChunks);
    for (const shardStats of conciseShardsStats) {
        let estDataPercent =
            (totals.size == 0) ? 0 : (Math.floor(shardStats.size / totals.size * 10000) / 100);
        let estDocPercent =
            (totals.count == 0) ? 0 : (Math.floor(shardStats.count / totals.count * 10000) / 100);

        print(" Shard " + shardStats.shardId + " contains " + estDataPercent + "% data, " +
              estDocPercent + "% docs in cluster, " +
              "avg obj size on shard : " + sh._dataFormat(shardStats.avgObjSize));
    }

    print("\n");
};

/**
 * Prints shard key for this collection
 */
DBCollection.prototype.getShardKey = function() {
    if (!this._isSharded()) {
        throw Error("Collection " + this + " is not sharded.");
    }

    let config = this.getDB().getSiblingDB("config");
    const coll = config.collections.findOne({_id: this._fullName});
    return coll.key;
};

/*

Generates split points for all chunks in the collection, based on the default maxChunkSize
> collection.getSplitKeysForChunks()

or alternately a specified chunk size in Mb.
> collection.getSplitKeysForChunks( 10 )

By default, the chunks are not split, the keys are just found. A splitter function is returned which
will actually do the splits.
> var splitter = collection.getSplitKeysForChunks()
> splitter()

*/

DBCollection.prototype.getSplitKeysForChunks = function(chunkSize) {
    let stats = this.stats();

    if (!stats.sharded) {
        print("Collection " + this + " is not sharded.");
        return;
    }

    let config = this.getDB().getSiblingDB("config");

    if (!chunkSize) {
        chunkSize = config.settings.findOne({_id: "chunksize"}).value;
        print("Chunk size not set, using default of " + chunkSize + "MB");
    } else {
        print("Using chunk size of " + chunkSize + "MB");
    }

    let shardDocs = config.shards.find().toArray();

    let allSplitPoints = {};
    let numSplits = 0;

    for (let i = 0; i < shardDocs.length; i++) {
        let shardDoc = shardDocs[i];
        let shard = shardDoc._id;
        let host = shardDoc.host;
        let sconn = new Mongo(host);

        let chunks = config.chunks.find({_id: sh._collRE(this), shard: shard}).toArray();

        print("\nGetting split points for chunks on shard " + shard + " at " + host);

        let splitPoints = [];

        for (let j = 0; j < chunks.length; j++) {
            let chunk = chunks[j];
            let result = sconn.getDB("admin").runCommand(
                {splitVector: this + "", min: chunk.min, max: chunk.max, maxChunkSize: chunkSize});
            if (!result.ok) {
                print(" Had trouble getting split keys for chunk " + sh._pchunk(chunk) + " :\n");
                printjson(result);
            } else {
                splitPoints = splitPoints.concat(result.splitKeys);

                if (result.splitKeys.length > 0)
                    print(" Added " + result.splitKeys.length + " split points for chunk " +
                          sh._pchunk(chunk));
            }
        }

        print("Total splits for shard " + shard + " : " + splitPoints.length);

        numSplits += splitPoints.length;
        allSplitPoints[shard] = splitPoints;
    }

    // Get most recent migration
    let migration = config.changelog.find({what: /^move.*/}).sort({time: -1}).limit(1).toArray();
    if (migration.length == 0)
        print("\nNo migrations found in changelog.");
    else {
        migration = migration[0];
        print("\nMost recent migration activity was on " + migration.ns + " at " + migration.time);
    }

    let admin = this.getDB().getSiblingDB("admin");
    let coll = this;
    let splitFunction = function() {
        // Turn off the balancer, just to be safe
        print("Turning off balancer...");
        config.settings.update({_id: "balancer"}, {$set: {stopped: true}}, true);
        print(
            "Sleeping for 30s to allow balancers to detect change.  To be extra safe, check config.changelog" +
            " for recent migrations.");
        sleep(30000);

        for (let shard in allSplitPoints) {
            for (let i = 0; i < allSplitPoints[shard].length; i++) {
                let splitKey = allSplitPoints[shard][i];
                print("Splitting at " + tojson(splitKey));
                printjson(admin.runCommand({split: coll + "", middle: splitKey}));
            }
        }

        print("Turning the balancer back on.");
        config.settings.update({_id: "balancer"}, {$set: {stopped: false}});
        sleep(1);
    };

    splitFunction.getSplitPoints = function() {
        return allSplitPoints;
    };

    print("\nGenerated " + numSplits + " split keys, run output function to perform splits.\n" +
          " ex : \n" +
          "  > var splitter = <collection>.getSplitKeysForChunks()\n" +
          "  > splitter() // Execute splits on cluster !\n");

    return splitFunction;
};

/**
 * Enable balancing for this collection. Uses the configureCollectionBalancing command
 * with the enableBalancing paramater if FCV >= 8.1 and directly writes to config.collections if FCV
 * < 8.1.
 * TODO: SERVER-94845 remove FCV check when 9.0 becomes the last LTS
 */
DBCollection.prototype.enableBalancing = function() {
    if (!this._isSharded()) {
        throw Error("Collection " + this + " is not sharded.");
    }

    let adminDb = this.getDB().getSiblingDB("admin");
    const fcvDoc = adminDb.runCommand({
        getParameter: 1,
        featureCompatibilityVersion: 1,
    });
    if (MongoRunner.compareBinVersions(
            fcvDoc.featureCompatibilityVersion.version,
            "8.1",
            ) >= 0) {
        return adminDb.runCommand(
            {configureCollectionBalancing: this._fullName, enableBalancing: true});
    } else {
        let configDb = this.getDB().getSiblingDB("config");
        return assert.commandWorked(
            configDb.collections.update({_id: this._fullName}, {$set: {"noBalance": false}}));
    }
};

/**
 * Disable balancing for this collection. Uses the configureCollectionBalancing command
 * with the enableBalancing paramater if FCV >= 8.1 and directly writes to config.collections if FCV
 * < 8.1.
 * TODO: SERVER-94845 remove FCV check when 9.0 becomes the last LTS
 */
DBCollection.prototype.disableBalancing = function() {
    if (!this._isSharded()) {
        throw Error("Collection " + this + " is not sharded.");
    }
    let adminDb = this.getDB().getSiblingDB("admin");
    const fcvDoc = adminDb.runCommand({
        getParameter: 1,
        featureCompatibilityVersion: 1,
    });
    if (MongoRunner.compareBinVersions(
            fcvDoc.featureCompatibilityVersion.version,
            "8.1",
            ) >= 0) {
        return adminDb.runCommand(
            {configureCollectionBalancing: this._fullName, enableBalancing: false});
    } else {
        let configDb = this.getDB().getSiblingDB("config");
        return assert.commandWorked(
            configDb.collections.update({_id: this._fullName}, {$set: {"noBalance": true}}));
    }
};

DBCollection.prototype.setSlaveOk = function(value) {
    print(
        "WARNING: setSlaveOk() is deprecated and may be removed in the next major release. Please use setSecondaryOk() instead.");
    this.setSecondaryOk(value);
};

DBCollection.prototype.getSlaveOk = function() {
    print(
        "WARNING: getSlaveOk() is deprecated and may be removed in the next major release. Please use getSecondaryOk() instead.");
    return this.getSecondaryOk();
};

DBCollection.prototype.setSecondaryOk = function(value = true) {
    this._secondaryOk = value;
};

DBCollection.prototype.getSecondaryOk = function() {
    if (this._secondaryOk !== undefined)
        return this._secondaryOk;
    return this._db.getSecondaryOk();
};

DBCollection.prototype.getQueryOptions = function() {
    // inherit this method from DB but use apply so
    // that secondaryOk will be set if is overridden on this DBCollection
    return this._db.getQueryOptions.apply(this, arguments);
};

/**
 * Returns a PlanCache for the collection.
 */
DBCollection.prototype.getPlanCache = function() {
    return new PlanCache(this);
};

// Overrides connection-level settings.
//

DBCollection.prototype.setWriteConcern = function(wc) {
    if (wc instanceof WriteConcern) {
        this._writeConcern = wc;
    } else {
        this._writeConcern = new WriteConcern(wc);
    }
};

DBCollection.prototype.getWriteConcern = function() {
    if (this._writeConcern)
        return this._writeConcern;

    if (this._db.getWriteConcern())
        return this._db.getWriteConcern();

    return null;
};

DBCollection.prototype.unsetWriteConcern = function() {
    delete this._writeConcern;
};

/**
 * disable auto-merging on this collection
 */
DBCollection.prototype.disableAutoMerger = function() {
    return this._db._adminCommand(
        {configureCollectionBalancing: this._fullName, enableAutoMerger: false});
};

/**
 * enable auto-merge on this collection
 */
DBCollection.prototype.enableAutoMerger = function() {
    return this._db._adminCommand(
        {configureCollectionBalancing: this._fullName, enableAutoMerger: true});
};

//
// CRUD specification read methods
//

/**
 * Count number of matching documents in the db to a query.
 *
 * @method
 * @param {object} query The query for the count.
 * @param {object} [options=null] Optional settings.
 * @param {number} [options.limit=null] The limit of documents to count.
 * @param {number} [options.skip=null] The number of documents to skip for the count.
 * @param {string|object} [options.hint=null] An index name hint or specification for the query.
 * @param {number} [options.maxTimeMS=null] The maximum amount of time to allow the query to run.
 * @param {string} [options.readConcern=null] The level of readConcern passed to the count command
 * @param {object} [options.collation=null] The collation that should be used for string comparisons
 * for this count op.
 * @param {boolean} [options.rawData=null] Whether to operate on the underlying data format of the
 *     collection.
 * @return {number}
 *
 */
DBCollection.prototype.count = function(query, options) {
    const cmd =
        Object.assign({count: this.getName(), query: this._massageObject(query || {})}, options);
    if (cmd.readConcern) {
        cmd.readConcern = {level: cmd.readConcern};
    }
    const res = this._db.runReadCommand(cmd);
    if (!res.ok) {
        throw _getErrorWithCode(res, "count failed: " + tojson(res));
    }
    return res.n;
};

/**
 * Count number of matching documents in the db to a query using aggregation.
 *
 * @method
 * @param {object} query The query for the count.
 * @param {object} [options=null] Optional settings.
 * @param {number} [options.limit=null] The limit of documents to count.
 * @param {number} [options.skip=null] The number of documents to skip for the count.
 * @param {string|object} [options.hint=null] An index name hint or specification for the query.
 * @param {number} [options.maxTimeMS=null] The maximum amount of time to allow the query to run.
 * @param {object} [options.collation=null] The collation that should be used for string comparisons
 * for this count op.
 * @param {boolean} [options.rawData=null] Whether to operate on the underlying data format of the
 *     collection.
 * @return {number}
 */
DBCollection.prototype.countDocuments = function(query, options) {
    "use strict";
    let pipeline = [{"$match": query}];
    options = options || {};
    assert.eq(typeof options, "object", "'options' argument must be an object");

    if (options.skip) {
        pipeline.push({"$skip": options.skip});
    }
    if (options.limit) {
        pipeline.push({"$limit": options.limit});
    }

    // Construct an aggregation pipeline stage with sum to calculate the number of all documents.
    pipeline.push({"$group": {"_id": null, "n": {"$sum": 1}}});

    // countDocument options other than filter, skip, and limit, are added to the aggregate command.
    let aggregateOptions = {};

    if (options.hint) {
        aggregateOptions.hint = options.hint;
    }
    if (options.maxTimeMS) {
        aggregateOptions.maxTimeMS = options.maxTimeMS;
    }
    if (options.collation) {
        aggregateOptions.collation = options.collation;
    }
    if (options.rawData) {
        aggregateOptions.rawData = options.rawData;
    }

    // Format cursor into an array.
    const res = this.aggregate(pipeline, aggregateOptions).toArray();
    if (res.length) {
        return res[0].n;
    }

    return 0;
};

/**
 * Estimates the count of documents in a collection using collection metadata.
 *
 * @method
 * @param {object} [options=null] Optional settings.
 * @param {number} [options.maxTimeMS=null] The maximum amount of time to allow the query to run.
 * @param {boolean} [options.rawData=null] Whether to operate on the underlying data format of the
 *     collection.
 * @return {number}
 */
DBCollection.prototype.estimatedDocumentCount = function(options) {
    "use strict";
    let cmd = {count: this.getName()};
    options = options || {};
    assert.eq(typeof options, "object", "'options' argument must be an object");

    if (options.maxTimeMS) {
        cmd.maxTimeMS = options.maxTimeMS;
    }
    if (options.rawData) {
        cmd.rawData = options.rawData;
    }

    const res = this.runCommand(cmd);

    if (!res.ok) {
        throw _getErrorWithCode(res, "Error estimating document count: " + tojson(res));
    }

    // Return the 'n' field, which should be the count of documents.
    return res.n;
};

/**
 * The distinct command returns returns a list of distinct values for the given key across a
 *collection.
 *
 * @method
 * @param {string} key Field of the document to find distinct values for.
 * @param {object} query The query for filtering the set of documents to which we apply the distinct
 *filter.
 * @param {object} [options=null] Optional settings.
 * @param {number} [options.maxTimeMS=null] The maximum amount of time to allow the query to run.
 * @return {object}
 */
DBCollection.prototype.distinct = function(keyString, query, options) {
    let opts = Object.extend({}, options || {});
    let keyStringType = typeof keyString;
    let queryType = typeof query;

    if (keyStringType != "string") {
        throw new Error("The first argument to the distinct command must be a string but was a " +
                        keyStringType);
    }

    if (query != null && queryType != "object") {
        throw new Error("The query argument to the distinct command must be a document but was a " +
                        queryType);
    }

    // Distinct command
    let cmd = {distinct: this.getName(), key: keyString, query: query || {}};

    // Set maxTimeMS if provided
    if (opts.maxTimeMS) {
        cmd.maxTimeMS = opts.maxTimeMS;
    }

    if (opts.collation) {
        cmd.collation = opts.collation;
    }

    if (opts.hint) {
        cmd.hint = opts.hint;
    }

    if (opts.rawData) {
        cmd.rawData = opts.rawData;
    }

    // Execute distinct command
    let res = this.runReadCommand(cmd);
    if (!res.ok) {
        throw _getErrorWithCode(res, "distinct failed: " + tojson(res));
    }

    return res.values;
};

DBCollection.prototype._distinct = function(keyString, query) {
    return this._dbReadCommand({distinct: this._shortName, key: keyString, query: query || {}});
};

DBCollection.prototype.latencyStats = function(options) {
    options = options || {};
    return this.aggregate([{$collStats: {latencyStats: options}}]);
};

DBCollection.prototype.watch = function(pipeline, options) {
    pipeline = pipeline || [];
    assert(pipeline instanceof Array, "'pipeline' argument must be an array");
    const [changeStreamStage, aggOptions] = this.getMongo()._extractChangeStreamOptions(options);
    return this.aggregate([changeStreamStage, ...pipeline], aggOptions);
};

DBCollection.prototype.checkMetadataConsistency = function(options = {}) {
    assert(options instanceof Object,
           `'options' parameter expected to be type object but found: ${typeof options}`);
    const res = assert.commandWorked(
        this._db.runCommand(Object.extend({checkMetadataConsistency: this.getName()}, options)));
    return new DBCommandCursor(this._db, res);
};

/**
 * PlanCache
 * Holds a reference to the collection.
 * Proxy for planCache* commands.
 */
if ((typeof PlanCache) == "undefined") {
    globalThis.PlanCache = function(collection) {
        this._collection = collection;
    };
}

/**
 * Name of PlanCache.
 * Same as collection.
 */
PlanCache.prototype.getName = function() {
    return this._collection.getName();
};

/**
 * toString prints the name of the collection
 */
PlanCache.prototype.toString = function() {
    return "PlanCache for collection " + this.getName() + '. Type help() for more info.';
};

PlanCache.prototype.shellPrint = PlanCache.prototype.toString;

/**
 * Displays help for a PlanCache object.
 */
PlanCache.prototype.help = function() {
    let shortName = this.getName();
    print("PlanCache help");
    print("\tdb." + shortName + ".getPlanCache().help() - show PlanCache help");
    print("\tdb." + shortName + ".getPlanCache().clear() - " +
          "drops all cached queries in a collection");
    print("\tdb." + shortName +
          ".getPlanCache().clearPlansByQuery(query[, projection, sort, collation]) - " +
          "drops query shape from plan cache");
    print("\tdb." + shortName + ".getPlanCache().list([pipeline]) - " +
          "displays a serialization of the plan cache for this collection, " +
          "after applying an optional aggregation pipeline");
    return __magicNoPrint;
};

/**
 * Internal function to parse query shape.
 */
PlanCache.prototype._parseQueryShape = function(query, projection, sort, collation) {
    if (query == undefined) {
        throw new Error("required parameter query missing");
    }

    // Accept query shape object as only argument.
    // Query shape must contain 'query', 'projection', and 'sort', and may optionally contain
    // 'collation'. 'collation' must be non-empty if present.
    if (typeof (query) == 'object' && projection == undefined && sort == undefined &&
        collation == undefined) {
        let keysSorted = Object.keys(query).sort();
        // Expected keys must be sorted for the comparison to work.
        if (bsonWoCompare(keysSorted, ['projection', 'query', 'sort']) == 0) {
            return query;
        }
        if (bsonWoCompare(keysSorted, ['collation', 'projection', 'query', 'sort']) == 0) {
            if (Object.keys(query.collation).length === 0) {
                throw new Error("collation object must not be empty");
            }
            return query;
        }
    }

    // Extract query shape, projection, sort and collation from DBQuery if it is the first
    // argument. If a sort or projection is provided in addition to DBQuery, do not
    // overwrite with the DBQuery value.
    if (query instanceof DBQuery) {
        if (projection != undefined) {
            throw new Error("cannot pass DBQuery with projection");
        }
        if (sort != undefined) {
            throw new Error("cannot pass DBQuery with sort");
        }
        if (collation != undefined) {
            throw new Error("cannot pass DBQuery with collation");
        }

        let queryObj = query._query["query"] || {};
        projection = query._fields || {};
        sort = query._query["orderby"] || {};
        collation = query._query["collation"] || undefined;
        // Overwrite DBQuery with the BSON query.
        query = queryObj;
    }

    let shape = {
        query: query,
        projection: projection == undefined ? {} : projection,
        sort: sort == undefined ? {} : sort,
    };

    if (collation !== undefined) {
        shape.collation = collation;
    }

    return shape;
};

/**
 * Internal function to run command.
 */
PlanCache.prototype._runCommandThrowOnError = function(cmd, params) {
    let res = this._collection.runCommand(cmd, params);
    if (!res.ok) {
        throw new Error(res.errmsg);
    }
    return res;
};

/**
 * Clears plan cache in a collection.
 */
PlanCache.prototype.clear = function() {
    this._runCommandThrowOnError("planCacheClear", {});
    return;
};

/**
 * Drop query shape from the plan cache.
 */
PlanCache.prototype.clearPlansByQuery = function(query, projection, sort, collation) {
    this._runCommandThrowOnError("planCacheClear",
                                 this._parseQueryShape(query, projection, sort, collation));
    return;
};

/**
 * Returns an array of plan cache data for the collection, after applying the given optional
 * aggregation pipeline.
 */
PlanCache.prototype.list = function(pipeline) {
    const additionalPipeline = pipeline || [];
    const completePipeline = [{$planCacheStats: {}}].concat(additionalPipeline);
    return this._collection.aggregate(completePipeline).toArray();
};
