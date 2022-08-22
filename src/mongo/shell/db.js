// db.js

var DB;

(function() {

var _defaultWriteConcern = {w: 'majority', wtimeout: 10 * 60 * 1000};
const kWireVersionSupportingScramSha256Fallback = 15;

if (DB === undefined) {
    DB = function(mongo, name) {
        this._mongo = mongo;
        this._name = name;
    };
}

/**
 * Rotate certificates, CRLs, and CA files.
 * @param {String} message optional message for server to log at rotation time
 */
DB.prototype.rotateCertificates = function(message) {
    return this._adminCommand({rotateCertificates: 1, message: message});
};

DB.prototype.getMongo = function() {
    assert(this._mongo, "why no mongo!");
    return this._mongo;
};

DB.prototype.getSiblingDB = function(name) {
    return this.getSession().getDatabase(name);
};

DB.prototype.getSisterDB = DB.prototype.getSiblingDB;

DB.prototype.getName = function() {
    return this._name;
};

/**
 * Gets DB level statistics. opt can be a number representing the scale for backwards compatibility
 * or a document with options passed along to the dbstats command.
 */
DB.prototype.stats = function(opt) {
    var cmd = {dbstats: 1};

    if (opt === undefined)
        return this.runCommand(cmd);
    if (typeof (opt) !== "object")
        return this.runCommand(Object.extend(cmd, {scale: opt}));

    return this.runCommand(Object.extend(cmd, opt));
};

DB.prototype.getCollection = function(name) {
    return new DBCollection(this._mongo, this, name, this._name + "." + name);
};

DB.prototype.commandHelp = function(name) {
    var c = {};
    c[name] = 1;
    c.help = true;
    var res = this.runCommand(c);
    if (!res.ok)
        throw _getErrorWithCode(res, res.errmsg);
    return res.help;
};

// utility to attach readPreference if needed.
DB.prototype._attachReadPreferenceToCommand = function(cmdObj, readPref) {
    "use strict";
    // if the user has not set a readpref, return the original cmdObj
    if ((readPref === null) || typeof (readPref) !== "object") {
        return cmdObj;
    }

    // if user specifies $readPreference manually, then don't change it
    if (cmdObj.hasOwnProperty("$readPreference")) {
        return cmdObj;
    }

    // copy object so we don't mutate the original
    var clonedCmdObj = Object.extend({}, cmdObj);
    // The server selection spec mandates that the key is '$query', but
    // the shell has historically used 'query'. The server accepts both,
    // so we maintain the existing behavior
    var cmdObjWithReadPref = {query: clonedCmdObj, $readPreference: readPref};
    return cmdObjWithReadPref;
};

/**
 * If someone passes i.e. runCommand("foo", {bar: "baz"}), we merge it in to
 * runCommand({foo: 1, bar: "baz"}).
 * If we already have a command object in the first argument, we ensure that the second
 * argument 'extraKeys' is either null or an empty object. This prevents users from accidentally
 * calling runCommand({foo: 1}, {bar: 1}) and expecting the final command invocation to be
 * runCommand({foo: 1, bar: 1}).
 * This helper abstracts that logic.
 */
DB.prototype._mergeCommandOptions = function(obj, extraKeys) {
    "use strict";

    if (typeof (obj) === "object") {
        if (Object.keys(extraKeys || {}).length > 0) {
            throw Error("Unexpected second argument to DB.runCommand(): (type: " +
                        typeof (extraKeys) + "): " + tojson(extraKeys));
        }
        return obj;
    } else if (typeof (obj) !== "string") {
        throw Error("First argument to DB.runCommand() must be either an object or a string: " +
                    "(type: " + typeof (obj) + "): " + tojson(obj));
    }

    var commandName = obj;
    var mergedCmdObj = {};
    mergedCmdObj[commandName] = 1;

    if (!extraKeys) {
        return mergedCmdObj;
    } else if (typeof (extraKeys) === "object") {
        // this will traverse the prototype chain of extra, but keeping
        // to maintain legacy behavior
        for (var key in extraKeys) {
            mergedCmdObj[key] = extraKeys[key];
        }
    } else {
        throw Error("Second argument to DB.runCommand(" + commandName +
                    ") must be an object: (type: " + typeof (extraKeys) +
                    "): " + tojson(extraKeys));
    }

    return mergedCmdObj;
};

// Like runCommand but applies readPreference if one has been set
// on the connection. Also sets slaveOk if a (non-primary) readPref has been set.
DB.prototype.runReadCommand = function(obj, extra, queryOptions) {
    "use strict";

    // Support users who call this function with a string commandName, e.g.
    // db.runReadCommand("commandName", {arg1: "value", arg2: "value"}).
    obj = this._mergeCommandOptions(obj, extra);
    queryOptions = queryOptions !== undefined ? queryOptions : this.getQueryOptions();

    {
        const session = this.getSession();

        const readPreference = session._getSessionAwareClient().getReadPreference(session);
        if (readPreference !== null) {
            obj = this._attachReadPreferenceToCommand(obj, readPreference);

            if (readPreference.mode !== "primary") {
                // Set slaveOk if readPrefMode has been explicitly set with a readPreference
                // other than primary.
                queryOptions |= 4;
            }
        }
    }

    // The 'extra' parameter is not used as we have already created a merged command object.
    return this.runCommand(obj, null, queryOptions);
};

// runCommand uses this impl to actually execute the command
DB.prototype._runCommandImpl = function(name, obj, options) {
    const session = this.getSession();
    return session._getSessionAwareClient().runCommand(session, name, obj, options);
};

DB.prototype.runCommand = function(obj, extra, queryOptions) {
    "use strict";

    // Support users who call this function with a string commandName, e.g.
    // db.runCommand("commandName", {arg1: "value", arg2: "value"}).
    var mergedObj = this._mergeCommandOptions(obj, extra);

    // if options were passed (i.e. because they were overridden on a collection), use them.
    // Otherwise use getQueryOptions.
    var options = (typeof (queryOptions) !== "undefined") ? queryOptions : this.getQueryOptions();

    try {
        return this._runCommandImpl(this._name, mergedObj, options);
    } catch (ex) {
        // When runCommand flowed through query, a connection error resulted in the message
        // "error doing query: failed". Even though this message is arguably incorrect
        // for a command failing due to a connection failure, we preserve it for backwards
        // compatibility. See SERVER-18334 for details.
        if (ex.hasOwnProperty("message") && ex.message.indexOf("network error") >= 0) {
            throw new Error("error doing query: failed: " + ex.message);
        }
        throw ex;
    }
};

DB.prototype._dbCommand = DB.prototype.runCommand;
DB.prototype._dbReadCommand = DB.prototype.runReadCommand;

DB.prototype.adminCommand = function(obj, extra) {
    if (this._name == "admin")
        return this.runCommand(obj, extra);
    return this.getSiblingDB("admin").runCommand(obj, extra);
};

DB.prototype._adminCommand = DB.prototype.adminCommand;  // alias old name

DB.prototype._helloOrLegacyHello = function(args) {
    let cmd = this.getMongo().getApiParameters().version ? {hello: 1} : {isMaster: 1};
    if (args) {
        Object.assign(cmd, args);
    }
    return this.runCommand(cmd);
};

DB.prototype._runCommandWithoutApiStrict = function(command) {
    let commandWithoutApiStrict = Object.assign({}, command);
    if (this.getMongo().getApiParameters().strict) {
        // Permit this command invocation, even if it's not in the requested API version.
        commandWithoutApiStrict["apiStrict"] = false;
    }

    return this.runCommand(commandWithoutApiStrict);
};

DB.prototype._runAggregate = function(cmdObj, aggregateOptions) {
    assert(cmdObj.pipeline instanceof Array, "cmdObj must contain a 'pipeline' array");
    assert(cmdObj.aggregate !== undefined, "cmdObj must contain 'aggregate' field");
    assert(aggregateOptions === undefined || aggregateOptions instanceof Object,
           "'aggregateOptions' argument must be an object");

    // Disallow explicit API parameters on the aggregate shell helper; callers should use runCommand
    // directly if they want to test this.
    assert.noAPIParams(aggregateOptions);

    // Make a copy of the initial command object, i.e. {aggregate: x, pipeline: [...]}.
    cmdObj = Object.extend({}, cmdObj);

    // Make a copy of the aggregation options.
    let optcpy = Object.extend({}, (aggregateOptions || {}));

    if ('batchSize' in optcpy) {
        if (optcpy.cursor == null) {
            optcpy.cursor = {};
        }

        optcpy.cursor.batchSize = optcpy['batchSize'];
        delete optcpy['batchSize'];
    } else if ('useCursor' in optcpy) {
        if (optcpy.cursor == null) {
            optcpy.cursor = {};
        }

        delete optcpy['useCursor'];
    }

    const maxAwaitTimeMS = optcpy.maxAwaitTimeMS;
    delete optcpy.maxAwaitTimeMS;

    // Reassign the cleaned-up options.
    aggregateOptions = optcpy;

    // Add the options to the command object.
    Object.extend(cmdObj, aggregateOptions);

    if (!('cursor' in cmdObj)) {
        cmdObj.cursor = {};
    }

    const res = this.runReadCommand(cmdObj);

    if (!res.ok && (res.code == 17020 || res.errmsg == "unrecognized field \"cursor") &&
        !("cursor" in aggregateOptions)) {
        // If the command failed because cursors aren't supported and the user didn't explicitly
        // request a cursor, try again without requesting a cursor.
        delete cmdObj.cursor;

        res = doAgg(cmdObj);

        if ('result' in res && !("cursor" in res)) {
            // convert old-style output to cursor-style output
            res.cursor = {ns: '', id: NumberLong(0)};
            res.cursor.firstBatch = res.result;
            delete res.result;
        }
    }

    assert.commandWorked(res, "aggregate failed");

    if ("cursor" in res) {
        let batchSizeValue = undefined;

        if (cmdObj["cursor"]["batchSize"] > 0) {
            batchSizeValue = cmdObj["cursor"]["batchSize"];
        }

        return new DBCommandCursor(this, res, batchSizeValue, maxAwaitTimeMS);
    }

    return res;
};

DB.prototype.aggregate = function(pipeline, aggregateOptions) {
    assert(pipeline instanceof Array, "pipeline argument must be an array");
    const cmdObj = this._mergeCommandOptions("aggregate", {pipeline: pipeline});

    return this._runAggregate(cmdObj, (aggregateOptions || {}));
};

/**
  Create a new collection in the database.  Normally, collection creation is automatic.  You
 would
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
    <li>
        storageEngine: BSON document containing storage engine specific options. Format:
                       {
                           storageEngine: {
                               storageEngine1: {
                                   ...
                               },
                               storageEngine2: {
                                   ...
                               },
                               ...
                           }
                       }
    </li>
    </ul>

    <p>Example:</p>
    <code>db.createCollection("movies", { size: 10 * 1024 * 1024, capped:true } );</code>

 * @param {String} name Name of new collection to create
 * @param {Object} options Object with options for call.  Options are listed above.
 * @return {Object} returned has member ok set to true if operation succeeds, false otherwise.
*/
DB.prototype.createCollection = function(name, opt) {
    var options = opt || {};

    var cmd = {create: name};
    Object.extend(cmd, options);

    return this._dbCommand(cmd);
};

/**
 * Command to create a view based on the specified aggregation pipeline.
 * Usage: db.createView(name, viewOn, pipeline: [{ $operator: {...}}, ... ])
 *
 *  @param name String - name of the new view to create
 *  @param viewOn String - name of the backing view or collection
 *  @param pipeline [{ $operator: {...}}, ... ] - the aggregation pipeline that defines the view
 *  @param options { } - options on the view, e.g., collations
 */
DB.prototype.createView = function(name, viewOn, pipeline, opt) {
    var options = opt || {};

    var cmd = {create: name};

    if (viewOn == undefined) {
        throw Error("Must specify a backing view or collection");
    }

    // Since we allow a single stage pipeline to be specified as an object
    // in aggregation, we need to account for that here for consistency.
    if (pipeline != undefined) {
        if (!Array.isArray(pipeline)) {
            pipeline = [pipeline];
        }
    }
    options.pipeline = pipeline;
    options.viewOn = viewOn;

    Object.extend(cmd, options);

    return this._dbCommand(cmd);
};

/**
 * @deprecated use getProfilingStatus
 *  Returns the current profiling level of this database
 *  @return SOMETHING_FIXME or null on error
 */
DB.prototype.getProfilingLevel = function() {
    var res = assert.commandWorked(this._dbCommand({profile: -1}));
    return res ? res.was : null;
};

/**
 *  @return the current profiling status
 *  example { was : 0, slowms : 100 }
 *  @return SOMETHING_FIXME or null on error
 */
DB.prototype.getProfilingStatus = function() {
    var res = this._dbCommand({profile: -1});
    if (!res.ok)
        throw _getErrorWithCode(res, "profile command failed: " + tojson(res));
    delete res.ok;
    return res;
};

/**
 * Erase the entire database.
 * @params writeConcern: (document) expresses the write concern of the drop command.
 * @return Object returned has member ok set to true if operation succeeds, false otherwise.
 */
DB.prototype.dropDatabase = function(writeConcern) {
    return this._dbCommand(
        {dropDatabase: 1, writeConcern: writeConcern ? writeConcern : _defaultWriteConcern});
};

/**
 * Shuts down the database.  Must be run while using the admin database.
 * @param opts Options for shutdown. Possible options are:
 *   - force: (boolean) if the server should shut down, even if there is no
 *     up-to-date secondary
 *   - timeoutSecs: (number) the server will continue checking over timeoutSecs
 *     if any other servers have caught up enough for it to shut down.
 */
DB.prototype.shutdownServer = function(opts) {
    if ("admin" != this._name) {
        return "shutdown command only works with the admin database; try 'use admin'";
    }

    var cmd = {'shutdown': 1};
    opts = opts || {};
    for (var o in opts) {
        cmd[o] = opts[o];
    }

    try {
        var res = this.runCommand(cmd);
        if (!res.ok) {
            throw _getErrorWithCode(res, 'shutdownServer failed: ' + tojson(res));
        }
        throw Error('shutdownServer failed: server is still up.');
    } catch (e) {
        // we expect the command to not return a response, as the server will shut down
        // immediately.
        if (isNetworkError(e)) {
            print('server should be down...');
            return;
        }
        throw e;
    }
};

DB.prototype.help = function() {
    print("DB methods:");
    print(
        "\tdb.adminCommand(nameOrDocument) - switches to 'admin' db, and runs command [just calls db.runCommand(...)]");
    print(
        "\tdb.aggregate([pipeline], {options}) - performs a collectionless aggregation on this database; returns a cursor");
    print("\tdb.auth(username, password)");
    print("\tdb.commandHelp(name) returns the help for the command");
    print("\tdb.createUser(userDocument)");
    print("\tdb.createView(name, viewOn, [{$operator: {...}}, ...], {viewOptions})");
    print("\tdb.currentOp() displays currently executing operations in the db");
    print("\tdb.dropDatabase(writeConcern)");
    print("\tdb.dropUser(username)");
    print("\tdb.eval() - deprecated");
    print("\tdb.fsyncLock() flush data to disk and lock server for backups");
    print("\tdb.fsyncUnlock() unlocks server following a db.fsyncLock()");
    print("\tdb.getCollection(cname) same as db['cname'] or db.cname");
    print("\tdb.getCollectionInfos([filter]) - returns a list that contains the names and options" +
          " of the db's collections");
    print("\tdb.getCollectionNames()");
    print("\tdb.getLogComponents()");
    print("\tdb.getMongo() get the server connection object");
    print("\tdb.getMongo().setSecondaryOk() allow queries on a replication secondary server");
    print("\tdb.getName()");
    print("\tdb.getProfilingLevel() - deprecated");
    print("\tdb.getProfilingStatus() - returns if profiling is on and slow threshold");
    print("\tdb.getReplicationInfo()");
    print("\tdb.getSiblingDB(name) get the db at the same server as this one");
    print(
        "\tdb.getWriteConcern() - returns the write concern used for any operations on this db, inherited from server object if set");
    print("\tdb.hostInfo() get details about the server's host");
    print("\tdb.isMaster() check replica primary status");
    print("\tdb.hello() check replica primary status");
    print("\tdb.killOp(opid) kills the current operation in the db");
    print("\tdb.listCommands() lists all the db commands");
    print("\tdb.loadServerScripts() loads all the scripts in db.system.js");
    print("\tdb.logout()");
    print("\tdb.printCollectionStats()");
    print("\tdb.printReplicationInfo()");
    print("\tdb.printShardingStatus()");
    print("\tdb.printSecondaryReplicationInfo()");
    print(
        "\tdb.rotateCertificates(message) - rotates certificates, CRLs, and CA files and logs an optional message");
    print(
        "\tdb.runCommand(cmdObj) run a database command.  if cmdObj is a string, turns it into {cmdObj: 1}");
    print("\tdb.serverStatus()");
    print("\tdb.setLogLevel(level,<component>)");
    print("\tdb.setProfilingLevel(level,slowms) 0=off 1=slow 2=all");
    print("\tdb.setVerboseShell(flag) display extra information in shell output");
    print(
        "\tdb.setWriteConcern(<write concern doc>) - sets the write concern for writes to the db");
    print("\tdb.shutdownServer()");
    print("\tdb.stats()");
    print(
        "\tdb.unsetWriteConcern(<write concern doc>) - unsets the write concern for writes to the db");
    print("\tdb.version() current version of the server");
    print("\tdb.watch() - opens a change stream cursor for a database to report on all " +
          " changes to its non-system collections.");
    return __magicNoPrint;
};

DB.prototype.printCollectionStats = function(scale) {
    if (arguments.length > 1) {
        print("printCollectionStats() has a single optional argument (scale)");
        return;
    }
    if (typeof scale != 'undefined') {
        if (typeof scale != 'number') {
            print("scale has to be a number >= 1");
            return;
        }
        if (scale < 1) {
            print("scale has to be >= 1");
            return;
        }
    }
    var mydb = this;
    this.getCollectionNames().forEach(function(z) {
        print(z);
        printjson(mydb.getCollection(z).stats(scale));
        print("---");
    });
};

/**
 * Configures settings for capturing operations inside the system.profile collection and in the
 * slow query log.
 *
 * The 'level' can be 0, 1, or 2:
 *  - 0 means that profiling is off and nothing will be written to system.profile.
 *  - 1 means that profiling is on for operations slower than the currently configured 'slowms'
 *    threshold (more on 'slowms' below).
 *  - 2 means that profiling is on for all operations, regardless of whether or not they are
 *    slower than 'slowms'.
 *
 * The 'options' parameter, if a number, is interpreted as the 'slowms' value to send to the
 * server. 'slowms' determines the threshold, in milliseconds, above which slow operations get
 * profiled at profiling level 1 or logged at logLevel 0.
 *
 * If 'options' is not a number, it is expected to be an object containing additional parameters
 * to get passed to the server. For example, db.setProfilingLevel(2, {foo: "bar"}) will issue
 * the command {profile: 2, foo: "bar"} to the server.
 */
DB.prototype.setProfilingLevel = function(level, options) {
    if (level < 0 || level > 2) {
        var errorText = "input level " + level + " is out of range [0..2]";
        var errorObject = new Error(errorText);
        errorObject['dbSetProfilingException'] = errorText;
        throw errorObject;
    }

    var cmd = {profile: level};
    if (isNumber(options)) {
        cmd.slowms = options;
    } else {
        cmd = Object.extend(cmd, options);
    }
    return assert.commandWorked(this._dbCommand(cmd));
};

/**
 * @deprecated
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
 * <code>{ dbEvalException: { retval: functionReturnValue, ok: num [, errno: num] [, errmsg:
 *str] } }</code>
 *
 * <p>Example: </p>
 * <code>print( "mycount: " + db.eval( function(){db.mycoll.find({},{_id:ObjId()}).length();}
 *);</code>
 *
 * @param {Function} jsfunction Javascript function to run on server.  Note this it not a
 *closure, but rather just "code".
 * @return result of your function, or null if error
 *
 */
DB.prototype.eval = function(jsfunction) {
    print("WARNING: db.eval is deprecated");

    var cmd = {$eval: jsfunction};
    if (arguments.length > 1) {
        cmd.args = Array.from(arguments).slice(1);
    }

    var res = this._dbCommand(cmd);

    if (!res.ok)
        throw _getErrorWithCode(res, tojson(res));

    return res.retval;
};

DB.prototype.dbEval = DB.prototype.eval;

/**
 * <p>
 *  An array of grouped items is returned.  The array must fit in RAM, thus this function is not
 * suitable when the return set is extremely large.
 * </p>
 * <p>
 * To order the grouped data, simply sort it client side upon return.
 * <p>
   Defaults
     cond may be null if you want to run against all rows in the collection
     keyf is a function which takes an object and returns the desired key.  set either key or
 keyf (not both).
 * </p>
 */
DB.prototype.groupeval = function(parmsObj) {
    var groupFunction = function() {
        var parms = args[0];
        var c = db[parms.ns].find(parms.cond || {});
        var map = new Map();
        var pks = parms.key ? Object.keySet(parms.key) : null;
        var pkl = pks ? pks.length : 0;
        var key = {};

        while (c.hasNext()) {
            var obj = c.next();
            if (pks) {
                for (var i = 0; i < pkl; i++) {
                    var k = pks[i];
                    key[k] = obj[k];
                }
            } else {
                key = parms.$keyf(obj);
            }

            var aggObj = map.get(key);
            if (aggObj == null) {
                var newObj = Object.extend({}, key);  // clone
                aggObj = Object.extend(newObj, parms.initial);
                map.put(key, aggObj);
            }
            parms.$reduce(obj, aggObj);
        }

        return map.values();
    };

    return this.eval(groupFunction, this._groupFixParms(parmsObj));
};

DB.prototype._groupFixParms = function(parmsObj) {
    var parms = Object.extend({}, parmsObj);

    if (parms.reduce) {
        parms.$reduce = parms.reduce;  // must have $ to pass to db
        delete parms.reduce;
    }

    if (parms.keyf) {
        parms.$keyf = parms.keyf;
        delete parms.keyf;
    }

    return parms;
};

DB.prototype.forceError = function() {
    return this.runCommand({forceerror: 1});
};

DB.prototype._getCollectionInfosCommand = function(
    filter, nameOnly = false, authorizedCollections = false, options = {}) {
    filter = filter || {};
    const cmd = {
        listCollections: 1,
        filter: filter,
        nameOnly: nameOnly,
        authorizedCollections: authorizedCollections
    };

    const res = this.runCommand(Object.merge(cmd, options));
    if (!res.ok) {
        throw _getErrorWithCode(res, "listCollections failed: " + tojson(res));
    }

    return new DBCommandCursor(this, res).toArray().sort(compareOn("name"));
};

DB.prototype._getCollectionInfosFromPrivileges = function() {
    let ret = this.runCommand({connectionStatus: 1, showPrivileges: 1});
    if (!ret.ok) {
        throw _getErrorWithCode(res, "Failed to acquire collection information from privileges");
    }

    // Parse apart collection information.
    let result = [];

    let privileges = ret.authInfo.authenticatedUserPrivileges;
    if (privileges === undefined) {
        return result;
    }

    privileges.forEach(privilege => {
        let resource = privilege.resource;
        if (resource === undefined) {
            return;
        }
        let db = resource.db;
        if (db === undefined || db !== this.getName()) {
            return;
        }
        let collection = resource.collection;
        if (collection === undefined || typeof collection !== "string" || collection === "") {
            return;
        }

        result.push({name: collection});
    });

    return result.sort(compareOn("name"));
};

/**
 * Returns a list that contains the names and options of this database's collections, sorted
 * by collection name. An optional filter can be specified to match only collections with
 * certain metadata.
 */
DB.prototype.getCollectionInfos = function(
    filter, nameOnly = false, authorizedCollections = false) {
    try {
        return this._getCollectionInfosCommand(filter, nameOnly, authorizedCollections);
    } catch (ex) {
        if (ex.code !== ErrorCodes.Unauthorized) {
            // We cannot recover from this error, propagate it.
            throw ex;
        }

        // We may be able to compute a set of *some* collections which exist and we have access
        // to from our privileges. For this to work, the previous operation must have failed due
        // to authorization, we must be attempting to recover the names of our own collections,
        // and no filter can have been provided.

        if (nameOnly && authorizedCollections && Object.getOwnPropertyNames(filter).length === 0 &&
            ex.code === ErrorCodes.Unauthorized) {
            print(
                "Warning: unable to run listCollections, attempting to approximate collection names by parsing connectionStatus");
            return this._getCollectionInfosFromPrivileges();
        }

        throw ex;
    }
};

DB.prototype._getCollectionNamesInternal = function(options) {
    return this._getCollectionInfosCommand({}, true, true, options).map(function(infoObj) {
        return infoObj.name;
    });
};

/**
 * Returns this database's list of collection names in sorted order.
 */
DB.prototype.getCollectionNames = function() {
    return this._getCollectionNamesInternal({});
};

DB.prototype.tojson = function() {
    return this._name;
};

DB.prototype.toString = function() {
    return this._name;
};

DB.prototype.isMaster = function() {
    return this.runCommand("isMaster");
};

DB.prototype.hello = function() {
    return this.runCommand("hello");
};

DB.prototype.currentOp = function(arg) {
    try {
        const results = this.currentOpCursor(arg).toArray();
        let res = {"inprog": results.length > 0 ? results : [], "ok": 1};
        Object.defineProperty(res, "fsyncLock", {
            get: function() {
                throw Error(
                    "fsyncLock is no longer included in the currentOp shell helper, run db.runCommand({currentOp: 1}) instead.");
            }
        });
        return res;
    } catch (e) {
        return {"ok": 0, "code": e.code, "errmsg": "Error executing $currentOp: " + e.message};
    }
};

DB.prototype.currentOpCursor = function(arg) {
    let q = {};
    if (arg) {
        if (typeof (arg) == "object")
            Object.extend(q, arg);
        else if (arg)
            q["$all"] = true;
    }

    // Convert the incoming currentOp command into an equivalent aggregate command
    // of the form {aggregate:1, pipeline: [{$currentOp: {idleConnections: $all, allUsers:
    // !$ownOps, truncateOps: false}}, {$match: {<user-defined filter>}}], cursor:{}}.
    let pipeline = [];

    let currOpArgs = {};
    let currOpStage = {"$currentOp": currOpArgs};
    currOpArgs["allUsers"] = !q["$ownOps"];
    currOpArgs["idleConnections"] = !!q["$all"];
    currOpArgs["truncateOps"] = false;

    pipeline.push(currOpStage);

    let matchArgs = {};
    let matchStage = {"$match": matchArgs};
    for (const fieldname of Object.keys(q)) {
        if (fieldname !== "$all" && fieldname !== "$ownOps" && fieldname !== "$truncateOps") {
            matchArgs[fieldname] = q[fieldname];
        }
    }

    pipeline.push(matchStage);

    // The legacy db.currentOp() shell helper ignored any explicitly set read preference and used
    // the default, with the ability to also run on secondaries. To preserve this behavior we will
    // run the aggregate with read preference "primaryPreferred".
    return this.getSiblingDB("admin").aggregate(pipeline,
                                                {"$readPreference": {"mode": "primaryPreferred"}});
};

DB.prototype.killOp = function(op) {
    if (!op)
        throw Error("no opNum to kill specified");
    return this.adminCommand({'killOp': 1, 'op': op});
};
DB.prototype.killOP = DB.prototype.killOp;

DB.tsToSeconds = function(x) {
    if (x.t && x.i)
        return x.t;
    return x / 4294967296;  // low 32 bits are ordinal #s within a second
};

/**
  Get a replication log information summary.
  <p>
  This command is for the database/cloud administer and not applicable to most databases.
  It is only used with the local database.  One might invoke from the JS shell:
  <pre>
       use local
       db.getReplicationInfo();
  </pre>
  * @return Object timeSpan: time span of the oplog from start to end  if secondary is more out
  *                          of date than that, it can't recover without a complete resync
*/
DB.prototype.getReplicationInfo = function() {
    var localdb = this.getSiblingDB("local");

    var result = {};
    var oplog;
    var localCollections = localdb.getCollectionNames();
    if (localCollections.indexOf('oplog.rs') >= 0) {
        oplog = 'oplog.rs';
    } else {
        result.errmsg = "replication not detected";
        return result;
    }

    var ol = localdb.getCollection(oplog);
    var ol_stats = ol.stats();
    if (ol_stats && ol_stats.maxSize) {
        result.logSizeMB = ol_stats.maxSize / (1024 * 1024);
    } else {
        result.errmsg = "Could not get stats for local." + oplog + " collection. " +
            "collstats returned: " + tojson(ol_stats);
        return result;
    }

    result.usedMB = ol_stats.size / (1024 * 1024);
    result.usedMB = Math.ceil(result.usedMB * 100) / 100;

    var firstc = ol.find().sort({$natural: 1}).limit(1);
    var lastc = ol.find().sort({$natural: -1}).limit(1);
    if (!firstc.hasNext() || !lastc.hasNext()) {
        result.errmsg =
            "objects not found in local.oplog.$main -- is this a new and empty db instance?";
        result.oplogMainRowCount = ol.count();
        return result;
    }

    var first = firstc.next();
    var last = lastc.next();
    var tfirst = first.ts;
    var tlast = last.ts;

    if (tfirst && tlast) {
        tfirst = DB.tsToSeconds(tfirst);
        tlast = DB.tsToSeconds(tlast);
        result.timeDiff = tlast - tfirst;
        result.timeDiffHours = Math.round(result.timeDiff / 36) / 100;
        result.tFirst = (new Date(tfirst * 1000)).toString();
        result.tLast = (new Date(tlast * 1000)).toString();
        result.now = Date();
    } else {
        result.errmsg = "ts element not found in oplog objects";
    }

    return result;
};

DB.prototype.printReplicationInfo = function() {
    var result = this.getReplicationInfo();
    if (result.errmsg) {
        let reply, isPrimary;
        if (this.getMongo().getApiParameters().apiVersion) {
            reply = this.hello();
            isPrimary = reply.isWritablePrimary;
        } else {
            reply = this.isMaster();
            isPrimary = reply.ismaster;
        }

        if (reply.arbiterOnly) {
            print("cannot provide replication status from an arbiter.");
            return;
        } else if (!isPrimary) {
            print("this is a secondary, printing secondary replication info.");
            this.printSecondaryReplicationInfo();
            return;
        }
        print(tojson(result));
        return;
    }
    print("configured oplog size:   " + result.logSizeMB + "MB");
    print("log length start to end: " + result.timeDiff + "secs (" + result.timeDiffHours + "hrs)");
    print("oplog first event time:  " + result.tFirst);
    print("oplog last event time:   " + result.tLast);
    print("now:                     " + result.now);
};

DB.prototype.printSlaveReplicationInfo = function() {
    print(
        "WARNING: printSlaveReplicationInfo is deprecated and may be removed in the next major release. Please use printSecondaryReplicationInfo instead.");
    this.printSecondaryReplicationInfo();
};

DB.prototype.printSecondaryReplicationInfo = function() {
    var startOptimeDate = null;
    var primary = null;

    function getReplLag(st) {
        assert(startOptimeDate, "how could this be null (getReplLag startOptimeDate)");
        print("\tsyncedTo: " + st.toString());
        var ago = (startOptimeDate - st) / 1000;
        var hrs = Math.round(ago / 36) / 100;
        var suffix = "";
        if (primary) {
            suffix = "primary ";
        } else {
            suffix = "freshest member (no primary available at the moment)";
        }
        print("\t" + Math.round(ago) + " secs (" + hrs + " hrs) behind the " + suffix);
    }

    function getPrimary(members) {
        for (i in members) {
            var row = members[i];
            if (row.state === 1) {
                return row;
            }
        }

        return null;
    }

    function printNodeReplicationInfo(node) {
        assert(node);
        if (node.state === 1 || node.state === 7) {  // ignore primaries (1) and arbiters (7)
            return;
        }

        print("source: " + node.name);
        if (node.optime && node.health != 0) {
            getReplLag(node.optimeDate);
        } else {
            print("\tno replication info, yet.  State: " + node.stateStr);
        }
    }

    function printNodeInitialSyncInfo(syncSourceString, remainingMillis) {
        print("\tInitialSyncSyncSource: " + syncSourceString);
        let minutes = Math.floor((remainingMillis / (1000 * 60)) % 60);
        let hours = Math.floor(remainingMillis / (1000 * 60 * 60));
        print("\tInitialSyncRemainingEstimatedDuration: " + hours + " hour(s) " + minutes +
              " minute(s)");
    }

    var L = this.getSiblingDB("local");

    if (L.system.replset.count() != 0) {
        const status =
            this.getSiblingDB('admin')._runCommandWithoutApiStrict({'replSetGetStatus': 1});
        primary = getPrimary(status.members);
        if (primary) {
            startOptimeDate = primary.optimeDate;
        }
        // no primary, find the most recent op among all members
        else {
            startOptimeDate = new Date(0, 0);
            for (i in status.members) {
                if (status.members[i].optimeDate > startOptimeDate) {
                    startOptimeDate = status.members[i].optimeDate;
                }
            }
        }

        for (i in status.members) {
            if (status.members[i].self && status.members[i].state === 5) {
                print("source: " + status.members[i].name);
                if (!status.initialSyncStatus) {
                    print("InitialSyncStatus information not found");
                    continue;
                }
                // Print initial sync info if node is in the STARTUP2 state.
                printNodeInitialSyncInfo(
                    status.members[i].syncSourceHost,
                    status.initialSyncStatus.remainingInitialSyncEstimatedMillis);
            } else {
                printNodeReplicationInfo(status.members[i]);
            }
        }
    }
};

DB.prototype.serverBuildInfo = function() {
    return this.getSiblingDB("admin")._runCommandWithoutApiStrict({buildinfo: 1});
};

// Used to trim entries from the metrics.commands that have never been executed
getActiveCommands = function(tree) {
    var result = {};
    for (var i in tree) {
        if (!tree.hasOwnProperty(i))
            continue;
        if (tree[i].hasOwnProperty("total")) {
            if (tree[i].total > 0) {
                result[i] = tree[i];
            }
            continue;
        }
        if (i == "<UNKNOWN>") {
            if (tree[i] > 0) {
                result[i] = tree[i];
            }
            continue;
        }
        // Handles nested commands
        var subStatus = getActiveCommands(tree[i]);
        if (Object.keys(subStatus).length > 0) {
            result[i] = tree[i];
        }
    }
    return result;
};

DB.prototype.serverStatus = function(options) {
    var cmd = {serverStatus: 1};
    if (options) {
        Object.extend(cmd, options);
    }
    var res = this._adminCommand(cmd);
    // Only prune if we have a metrics tree with commands.
    if (res.metrics && res.metrics.commands) {
        res.metrics.commands = getActiveCommands(res.metrics.commands);
    }
    return res;
};

DB.prototype.hostInfo = function() {
    return this._adminCommand("hostInfo");
};

DB.prototype.serverCmdLineOpts = function() {
    return this._adminCommand("getCmdLineOpts");
};

DB.prototype.version = function() {
    return this.serverBuildInfo().version;
};

DB.prototype.serverBits = function() {
    return this.serverBuildInfo().bits;
};

DB.prototype.listCommands = function() {
    var x = this.runCommand("listCommands");
    for (var name in x.commands) {
        var c = x.commands[name];

        var s = name + ": ";

        if (c.adminOnly)
            s += " adminOnly ";
        if (c.secondaryOk)
            s += " secondaryOk ";

        s += "\n  ";
        s += c.help.replace(/\n/g, '\n  ');
        s += "\n";

        print(s);
    }
};

DB.prototype.printShardingStatus = function(verbose) {
    printShardingStatus(this.getSiblingDB("config"), verbose);
};

DB.prototype.fsyncLock = function() {
    return this.adminCommand({fsync: 1, lock: true});
};

DB.prototype.fsyncUnlock = function() {
    return this.adminCommand({fsyncUnlock: 1});
};

DB.autocomplete = function(obj) {
    // In interactive mode, time out if a transaction or other op holds locks we need. Caller
    // suppresses exceptions. In non-interactive mode, don't specify a timeout, because in an
    // automated test we prefer consistent results over quick feedback.
    var colls = obj._getCollectionNamesInternal(isInteractive() ? {maxTimeMS: 1000} : {});
    var ret = [];
    for (var i = 0; i < colls.length; i++) {
        if (colls[i].match(/^[a-zA-Z0-9_.\$]+$/))
            ret.push(colls[i]);
    }
    return ret;
};

DB.prototype.setSlaveOk = function(value = true) {
    print(
        "WARNING: setSlaveOk() is deprecated and may be removed in the next major release. Please use setSecondaryOk() instead.");
    this.setSecondaryOk(value);
};

DB.prototype.getSlaveOk = function() {
    print(
        "WARNING: getSlaveOk() is deprecated and may be removed in the next major release. Please use getSecondaryOk() instead.");
    return this.getSecondaryOk();
};

DB.prototype.setSecondaryOk = function(value = true) {
    this._secondaryOk = value;
};

DB.prototype.getSecondaryOk = function() {
    if (this._secondaryOk != undefined)
        return this._secondaryOk;
    return this._mongo.getSecondaryOk();
};

DB.prototype.getQueryOptions = function() {
    var options = 0;
    if (this.getSecondaryOk())
        options |= 4;
    return options;
};

/* Loads any scripts contained in system.js into the client shell.
 */
DB.prototype.loadServerScripts = function() {
    var global = Function('return this')();
    this.system.js.find().forEach(function(u) {
        if (u.value.constructor === Code) {
            global[u._id] = eval("(" + u.value.code + ")");
        } else {
            global[u._id] = u.value;
        }
    });
};

////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////// Security shell helpers below
/////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

function getUserObjString(userObj) {
    var pwd = userObj.pwd;
    delete userObj.pwd;
    var toreturn = tojson(userObj);
    userObj.pwd = pwd;
    return toreturn;
}

DB.prototype._modifyCommandToDigestPasswordIfNecessary = function(cmdObj, username) {
    if (!cmdObj["pwd"]) {
        return;
    }
    if (cmdObj.hasOwnProperty("digestPassword")) {
        throw Error("Cannot specify 'digestPassword' through the user management shell helpers, " +
                    "use 'passwordDigestor' instead");
    }
    var passwordDigestor = cmdObj["passwordDigestor"] ? cmdObj["passwordDigestor"] : "server";
    if (passwordDigestor == "server") {
        cmdObj["digestPassword"] = true;
    } else if (passwordDigestor == "client") {
        cmdObj["pwd"] = _hashPassword(username, cmdObj["pwd"]);
        cmdObj["digestPassword"] = false;
    } else {
        throw Error("'passwordDigestor' must be either 'server' or 'client', got: '" +
                    passwordDigestor + "'");
    }
    delete cmdObj["passwordDigestor"];
};

DB.prototype.createUser = function(userObj, writeConcern) {
    var name = userObj["user"];
    if (name === undefined) {
        throw Error("no 'user' field provided to 'createUser' function");
    }

    if (userObj["createUser"] !== undefined) {
        throw Error("calling 'createUser' function with 'createUser' field is disallowed");
    }

    var cmdObj = {createUser: name};
    cmdObj = Object.extend(cmdObj, userObj);
    delete cmdObj["user"];

    this._modifyCommandToDigestPasswordIfNecessary(cmdObj, name);

    cmdObj["writeConcern"] = writeConcern ? writeConcern : _defaultWriteConcern;

    var res = this.runCommand(cmdObj);

    if (res.ok) {
        print("Successfully added user: " + getUserObjString(userObj));
        return;
    }

    if (res.errmsg == "no such cmd: createUser") {
        throw Error("'createUser' command not found.  This is most likely because you are " +
                    "talking to an old (pre v2.6) MongoDB server");
    }

    if (res.errmsg == "timeout") {
        throw Error("timed out while waiting for user authentication to replicate - " +
                    "database will not be fully secured until replication finishes");
    }

    throw _getErrorWithCode(res, "couldn't add user: " + res.errmsg);
};

function _hashPassword(username, password) {
    if (typeof password != 'string') {
        throw Error("User passwords must be of type string. Was given password with type: " +
                    typeof (password));
    }
    return hex_md5(username + ":mongo:" + password);
}

DB.prototype.updateUser = function(name, updateObject, writeConcern) {
    var cmdObj = {updateUser: name};
    cmdObj = Object.extend(cmdObj, updateObject);
    cmdObj['writeConcern'] = writeConcern ? writeConcern : _defaultWriteConcern;
    this._modifyCommandToDigestPasswordIfNecessary(cmdObj, name);

    var res = this.runCommand(cmdObj);
    if (res.ok) {
        return;
    }

    throw _getErrorWithCode(res, "Updating user failed: " + res.errmsg);
};

DB.prototype.changeUserPassword = function(username, password, writeConcern) {
    this.updateUser(username, {pwd: password}, writeConcern);
};

DB.prototype.logout = function() {
    // Logging out doesn't require a session since it manipulates connection state.
    return this.getMongo().logout(this.getName());
};

// For backwards compatibility
DB.prototype.removeUser = function(username, writeConcern) {
    print("WARNING: db.removeUser has been deprecated, please use db.dropUser instead");
    return this.dropUser(username, writeConcern);
};

DB.prototype.dropUser = function(username, writeConcern) {
    var cmdObj = {
        dropUser: username,
        writeConcern: writeConcern ? writeConcern : _defaultWriteConcern
    };
    var res = this.runCommand(cmdObj);

    if (res.ok) {
        return true;
    }

    if (res.code == 11) {  // Code 11 = UserNotFound
        return false;
    }

    throw _getErrorWithCode(res, res.errmsg);
};

DB.prototype.dropAllUsers = function(writeConcern) {
    var res = this.runCommand({
        dropAllUsersFromDatabase: 1,
        writeConcern: writeConcern ? writeConcern : _defaultWriteConcern
    });

    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }

    return res.n;
};

DB.prototype.__pwHash = function(nonce, username, pass) {
    return hex_md5(nonce + username + _hashPassword(username, pass));
};

DB.prototype._defaultAuthenticationMechanism = null;

function _fallbackToScramSha256(helloResult) {
    return helloResult && isNumber(helloResult.maxWireVersion) &&
        helloResult.maxWireVersion >= kWireVersionSupportingScramSha256Fallback;
}

DB.prototype._getDefaultAuthenticationMechanism = function(username, database) {
    let result = null;
    if (username !== undefined) {
        const userid = database + "." + username;
        result = this._helloOrLegacyHello({saslSupportedMechs: userid});

        if (result.ok && (result.saslSupportedMechs !== undefined)) {
            const mechs = result.saslSupportedMechs;
            if (!Array.isArray(mechs)) {
                throw Error("Server replied with invalid saslSupportedMechs response");
            }

            if ((this._defaultAuthenticationMechanism != null) &&
                mechs.includes(this._defaultAuthenticationMechanism)) {
                return this._defaultAuthenticationMechanism;
            }

            // Never include PLAIN in auto-negotiation.
            const priority = ["GSSAPI", "SCRAM-SHA-256", "SCRAM-SHA-1"];
            for (var i = 0; i < priority.length; ++i) {
                if (mechs.includes(priority[i])) {
                    return priority[i];
                }
            }
        }
        // If isMaster doesn't support saslSupportedMechs,
        // or if we couldn't agree on a mechanism,
        // then fall through to a default mech, either
        // configured or implicit based on the wire version
    }

    // Use the default auth mechanism if set on the command line.
    if (this._defaultAuthenticationMechanism != null) {
        return this._defaultAuthenticationMechanism;
    }

    // for later wire versions, we prefer (or require) SCRAM-SHA-256
    // if a fallback is required
    return _fallbackToScramSha256(result) ? "SCRAM-SHA-256" : "SCRAM-SHA-1";
};

DB.prototype._defaultGssapiServiceName = null;

DB.prototype._authOrThrow = function() {
    var params;
    if (arguments.length == 2) {
        params = {user: arguments[0], pwd: arguments[1]};
    } else if (arguments.length == 1) {
        if (typeof (arguments[0]) === "string") {
            let password = passwordPrompt();
            params = {user: arguments[0], pwd: password};
        } else if (typeof (arguments[0]) === "object") {
            params = Object.extend({}, arguments[0]);
        } else {
            throw Error("Single-argument form of auth expects a parameter object");
        }
    } else {
        throw Error(
            "auth expects (username), (username, password), or ({ user: username, pwd: password })");
    }

    if (params.mechanism === undefined) {
        params.mechanism = this._getDefaultAuthenticationMechanism(params.user, this.getName());
    }

    if (params.db !== undefined) {
        throw Error("Do not override db field on db.auth(). Use getMongo().auth(), instead.");
    }

    if (params.mechanism == "GSSAPI" && params.serviceName == null &&
        this._defaultGssapiServiceName != null) {
        params.serviceName = this._defaultGssapiServiceName;
    }

    // Logging in doesn't require a session since it manipulates connection state.
    params.db = this.getName();
    var good = this.getMongo().auth(params);
    if (good) {
        // auth enabled, and should try to use hello and replSetGetStatus to build prompt
        this.getMongo().authStatus = {authRequired: true, hello: true, replSetGetStatus: true};
    }

    return good;
};

DB.prototype.auth = function() {
    var ex;
    try {
        this._authOrThrow.apply(this, arguments);
    } catch (ex) {
        print(ex);
        return 0;
    }
    return 1;
};

DB.prototype.grantRolesToUser = function(username, roles, writeConcern) {
    var cmdObj = {
        grantRolesToUser: username,
        roles: roles,
        writeConcern: writeConcern ? writeConcern : _defaultWriteConcern
    };
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }
};

DB.prototype.revokeRolesFromUser = function(username, roles, writeConcern) {
    var cmdObj = {
        revokeRolesFromUser: username,
        roles: roles,
        writeConcern: writeConcern ? writeConcern : _defaultWriteConcern
    };
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }
};

DB.prototype.getUser = function(username, args) {
    if (typeof username != "string") {
        throw Error("User name for getUser shell helper must be a string");
    }
    var cmdObj = {usersInfo: username};
    Object.extend(cmdObj, args);

    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }

    if (res.users.length == 0) {
        return null;
    }
    return res.users[0];
};

DB.prototype.getUsers = function(args) {
    var cmdObj = {usersInfo: 1};
    Object.extend(cmdObj, args);
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        var authSchemaIncompatibleCode = 69;
        if (res.code == authSchemaIncompatibleCode ||
            (res.code == null && res.errmsg == "no such cmd: usersInfo")) {
            // Working with 2.4 schema user data
            return this.system.users.find({}).toArray();
        }

        throw _getErrorWithCode(res, res.errmsg);
    }

    return res.users;
};

DB.prototype.createRole = function(roleObj, writeConcern) {
    var name = roleObj["role"];
    var cmdObj = {createRole: name};
    cmdObj = Object.extend(cmdObj, roleObj);
    delete cmdObj["role"];
    cmdObj["writeConcern"] = writeConcern ? writeConcern : _defaultWriteConcern;

    var res = this.runCommand(cmdObj);

    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }
    printjson(roleObj);
};

DB.prototype.updateRole = function(name, updateObject, writeConcern) {
    var cmdObj = {updateRole: name};
    cmdObj = Object.extend(cmdObj, updateObject);
    cmdObj['writeConcern'] = writeConcern ? writeConcern : _defaultWriteConcern;
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }
};

DB.prototype.dropRole = function(name, writeConcern) {
    var cmdObj = {dropRole: name, writeConcern: writeConcern ? writeConcern : _defaultWriteConcern};
    var res = this.runCommand(cmdObj);

    if (res.ok) {
        return true;
    }

    if (res.code == 31) {  // Code 31 = RoleNotFound
        return false;
    }

    throw _getErrorWithCode(res, res.errmsg);
};

DB.prototype.dropAllRoles = function(writeConcern) {
    var res = this.runCommand({
        dropAllRolesFromDatabase: 1,
        writeConcern: writeConcern ? writeConcern : _defaultWriteConcern
    });

    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }

    return res.n;
};

DB.prototype.grantRolesToRole = function(rolename, roles, writeConcern) {
    var cmdObj = {
        grantRolesToRole: rolename,
        roles: roles,
        writeConcern: writeConcern ? writeConcern : _defaultWriteConcern
    };
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }
};

DB.prototype.revokeRolesFromRole = function(rolename, roles, writeConcern) {
    var cmdObj = {
        revokeRolesFromRole: rolename,
        roles: roles,
        writeConcern: writeConcern ? writeConcern : _defaultWriteConcern
    };
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }
};

DB.prototype.grantPrivilegesToRole = function(rolename, privileges, writeConcern) {
    var cmdObj = {
        grantPrivilegesToRole: rolename,
        privileges: privileges,
        writeConcern: writeConcern ? writeConcern : _defaultWriteConcern
    };
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }
};

DB.prototype.revokePrivilegesFromRole = function(rolename, privileges, writeConcern) {
    var cmdObj = {
        revokePrivilegesFromRole: rolename,
        privileges: privileges,
        writeConcern: writeConcern ? writeConcern : _defaultWriteConcern
    };
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }
};

DB.prototype.getRole = function(rolename, args) {
    if (typeof rolename != "string") {
        throw Error("Role name for getRole shell helper must be a string");
    }
    var cmdObj = {rolesInfo: rolename};
    Object.extend(cmdObj, args);
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }

    if (res.roles.length == 0) {
        return null;
    }
    return res.roles[0];
};

DB.prototype.getRoles = function(args) {
    var cmdObj = {rolesInfo: 1};
    Object.extend(cmdObj, args);
    var res = this.runCommand(cmdObj);
    if (!res.ok) {
        throw _getErrorWithCode(res, res.errmsg);
    }

    return res.roles;
};

DB.prototype.setWriteConcern = function(wc) {
    if (wc instanceof WriteConcern) {
        this._writeConcern = wc;
    } else {
        this._writeConcern = new WriteConcern(wc);
    }
};

DB.prototype.getWriteConcern = function() {
    if (this._writeConcern)
        return this._writeConcern;

    {
        const session = this.getSession();
        return session._getSessionAwareClient().getWriteConcern(session);
    }
};

DB.prototype.unsetWriteConcern = function() {
    delete this._writeConcern;
};

DB.prototype.getLogComponents = function() {
    return this.getMongo().getLogComponents(this.getSession());
};

DB.prototype.setLogLevel = function(logLevel, component) {
    return this.getMongo().setLogLevel(logLevel, component, this.getSession());
};

DB.prototype.watch = function(pipeline, options) {
    pipeline = pipeline || [];
    assert(pipeline instanceof Array, "'pipeline' argument must be an array");

    let changeStreamStage;
    [changeStreamStage, aggOptions] = this.getMongo()._extractChangeStreamOptions(options);
    pipeline.unshift(changeStreamStage);
    return this._runAggregate({aggregate: 1, pipeline: pipeline}, aggOptions);
};

DB.prototype.getFreeMonitoringStatus = function() {
    'use strict';
    return assert.commandWorked(this.adminCommand({getFreeMonitoringStatus: 1}));
};

DB.prototype.enableFreeMonitoring = function() {
    'use strict';
    let reply, isPrimary;
    if (this.getMongo().getApiParameters().apiVersion) {
        reply = this.hello();
        isPrimary = reply.isWritablePrimary;
    } else {
        reply = this.isMaster();
        isPrimary = reply.ismaster;
    }

    if (!isPrimary) {
        print("ERROR: db.enableFreeMonitoring() may only be run on a primary");
        return;
    }

    assert.commandWorked(this.adminCommand({setFreeMonitoring: 1, action: 'enable'}));

    const cmd = this.adminCommand({getFreeMonitoringStatus: 1});
    if (!cmd.ok && (cmd.code == ErrorCode.Unauthorized)) {
        // Edge case: It's technically possible that a user can change free-mon state,
        // but is not allowed to inspect it.
        print("Successfully initiated free monitoring, but unable to determine status " +
              "as you lack the 'checkFreeMonitoringStatus' privilege.");
        return;
    }
    assert.commandWorked(cmd);

    if (cmd.state !== 'enabled') {
        const url = this.adminCommand({'getParameter': 1, 'cloudFreeMonitoringEndpointURL': 1})
                        .cloudFreeMonitoringEndpointURL;

        print("Unable to get immediate response from the Cloud Monitoring service. We will" +
              "continue to retry in the background. Please check your firewall " +
              "settings to ensure that mongod can communicate with \"" + url + "\"");
        return;
    }

    print(tojson(cmd));
};

DB.prototype.disableFreeMonitoring = function() {
    'use strict';
    assert.commandWorked(this.adminCommand({setFreeMonitoring: 1, action: 'disable'}));
};

// Writing `this.hasOwnProperty` would cause DB.prototype.getCollection() to be called since the
// DB's getProperty() handler in C++ takes precedence when a property isn't defined on the DB
// instance directly. The "hasOwnProperty" property is defined on Object.prototype, so we must
// resort to using the function explicitly ourselves.
(function(hasOwnProperty) {
DB.prototype.getSession = function() {
    if (!hasOwnProperty.call(this, "_session")) {
        this._session = this.getMongo()._getDefaultSession();
    }
    return this._session;
};
})(Object.prototype.hasOwnProperty);

DB.prototype.createEncryptedCollection = function(name, opts) {
    assert.neq(
        opts, undefined, `createEncryptedCollection expected an opts object, it is undefined`);
    assert(opts.hasOwnProperty("encryptedFields") && typeof opts.encryptedFields == "object",
           `opts must contain an encryptedFields document'`);

    const res = assert.commandWorked(this.createCollection(name, opts));

    const cis = this.getCollectionInfos({"name": name});
    assert.eq(cis.length, 1, `Expected to find one collection named '${name}'`);

    const ci = cis[0];
    assert(ci.hasOwnProperty("options"), `Expected collection '${name}' to have 'options'`);
    const options = ci.options;
    assert(options.hasOwnProperty("encryptedFields"),
           `Expected collection '${name}' to have 'encryptedFields'`);
    const ef = options.encryptedFields;

    assert.commandWorked(this.getCollection(name).createIndex({__safeContent__: 1}));

    assert.commandWorked(
        this.createCollection(ef.escCollection, {clusteredIndex: {key: {_id: 1}, unique: true}}));
    assert.commandWorked(
        this.createCollection(ef.eccCollection, {clusteredIndex: {key: {_id: 1}, unique: true}}));
    assert.commandWorked(
        this.createCollection(ef.ecocCollection, {clusteredIndex: {key: {_id: 1}, unique: true}}));

    return res;
};

DB.prototype.dropEncryptedCollection = function(name) {
    const ci = db.getCollectionInfos({name: name})[0];
    if (ci == undefined) {
        throw `Encrypted Collection '${name}' not found`;
    }

    const ef = ci.options.encryptedFields;
    if (ef == undefined) {
        throw `Encrypted Collection '${name}' not found`;
    }

    this.getCollection(ef.escCollection).drop();
    this.getCollection(ef.eccCollection).drop();
    this.getCollection(ef.ecocCollection).drop();
    return this.getCollection(name).drop();
};
}());
