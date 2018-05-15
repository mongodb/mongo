// mongo.js

// NOTE 'Mongo' may be defined here or in MongoJS.cpp.  Add code to init, not to this constructor.
if (typeof Mongo == "undefined") {
    Mongo = function(host) {
        this.init(host);
    };
}

if (!Mongo.prototype) {
    throw Error("Mongo.prototype not defined");
}

if (!Mongo.prototype.find)
    Mongo.prototype.find = function(ns, query, fields, limit, skip, batchSize, options) {
        throw Error("find not implemented");
    };
if (!Mongo.prototype.insert)
    Mongo.prototype.insert = function(ns, obj) {
        throw Error("insert not implemented");
    };
if (!Mongo.prototype.remove)
    Mongo.prototype.remove = function(ns, pattern) {
        throw Error("remove not implemented");
    };
if (!Mongo.prototype.update)
    Mongo.prototype.update = function(ns, query, obj, upsert) {
        throw Error("update not implemented");
    };

if (typeof mongoInject == "function") {
    mongoInject(Mongo.prototype);
}

Mongo.prototype.setSlaveOk = function(value) {
    if (value == undefined)
        value = true;
    this.slaveOk = value;
};

Mongo.prototype.getSlaveOk = function() {
    return this.slaveOk || false;
};

Mongo.prototype.getDB = function(name) {
    if ((jsTest.options().keyFile) &&
        ((typeof this.authenticated == 'undefined') || !this.authenticated)) {
        jsTest.authenticate(this);
    }
    // There is a weird issue where typeof(db._name) !== "string" when the db name
    // is created from objects returned from native C++ methods.
    // This hack ensures that the db._name is always a string.
    if (typeof(name) === "object") {
        name = name.toString();
    }
    return new DB(this, name);
};

Mongo.prototype.getDBs = function(driverSession = this._getDefaultSession()) {
    var cmdObj = {listDatabases: 1};
    cmdObj = driverSession._serverSession.injectSessionId(cmdObj);

    var res = this.adminCommand(cmdObj);
    if (!res.ok)
        throw _getErrorWithCode(res, "listDatabases failed:" + tojson(res));
    return res;
};

Mongo.prototype.adminCommand = function(cmd) {
    return this.getDB("admin").runCommand(cmd);
};

/**
 * Returns all log components and current verbosity values
 */
Mongo.prototype.getLogComponents = function(driverSession = this._getDefaultSession()) {
    var cmdObj = {getParameter: 1, logComponentVerbosity: 1};
    cmdObj = driverSession._serverSession.injectSessionId(cmdObj);

    var res = this.adminCommand(cmdObj);
    if (!res.ok)
        throw _getErrorWithCode(res, "getLogComponents failed:" + tojson(res));
    return res.logComponentVerbosity;
};

/**
 * Accepts optional second argument "component",
 * string of form "storage.journaling"
 */
Mongo.prototype.setLogLevel = function(
    logLevel, component, driverSession = this._getDefaultSession()) {
    componentNames = [];
    if (typeof component === "string") {
        componentNames = component.split(".");
    } else if (component !== undefined) {
        throw Error("setLogLevel component must be a string:" + tojson(component));
    }
    var vDoc = {verbosity: logLevel};

    // nest vDoc
    for (var key, obj; componentNames.length > 0;) {
        obj = {};
        key = componentNames.pop();
        obj[key] = vDoc;
        vDoc = obj;
    }

    var cmdObj = {setParameter: 1, logComponentVerbosity: vDoc};
    cmdObj = driverSession._serverSession.injectSessionId(cmdObj);

    var res = this.adminCommand(cmdObj);
    if (!res.ok)
        throw _getErrorWithCode(res, "setLogLevel failed:" + tojson(res));
    return res;
};

Mongo.prototype.getDBNames = function() {
    return this.getDBs().databases.map(function(z) {
        return z.name;
    });
};

Mongo.prototype.getCollection = function(ns) {
    var idx = ns.indexOf(".");
    if (idx < 0)
        throw Error("need . in ns");
    var db = ns.substring(0, idx);
    var c = ns.substring(idx + 1);
    return this.getDB(db).getCollection(c);
};

Mongo.prototype.toString = function() {
    return "connection to " + this.host;
};
Mongo.prototype.tojson = Mongo.prototype.toString;

/**
 * Sets the read preference.
 *
 * @param mode {string} read preference mode to use. Pass null to disable read
 *     preference.
 * @param tagSet {Array.<Object>} optional. The list of tags to use, order matters.
 *     Note that this object only keeps a shallow copy of this array.
 */
Mongo.prototype.setReadPref = function(mode, tagSet) {
    if ((this._readPrefMode === "primary") && (typeof(tagSet) !== "undefined") &&
        (Object.keys(tagSet).length > 0)) {
        // we allow empty arrays/objects or no tagSet for compatibility reasons
        throw Error("Can not supply tagSet with readPref mode primary");
    }
    this._setReadPrefUnsafe(mode, tagSet);
};

// Set readPref without validating. Exposed so we can test the server's readPref validation.
Mongo.prototype._setReadPrefUnsafe = function(mode, tagSet) {
    this._readPrefMode = mode;
    this._readPrefTagSet = tagSet;
};

Mongo.prototype.getReadPrefMode = function() {
    return this._readPrefMode;
};

Mongo.prototype.getReadPrefTagSet = function() {
    return this._readPrefTagSet;
};

// Returns a readPreference object of the type expected by mongos.
Mongo.prototype.getReadPref = function() {
    var obj = {}, mode, tagSet;
    if (typeof(mode = this.getReadPrefMode()) === "string") {
        obj.mode = mode;
    } else {
        return null;
    }
    // Server Selection Spec: - if readPref mode is "primary" then the tags field MUST
    // be absent. Ensured by setReadPref.
    if (Array.isArray(tagSet = this.getReadPrefTagSet())) {
        obj.tags = tagSet;
    }

    return obj;
};

/**
 * Sets the read concern.
 *
 * @param level {string} read concern level to use. Pass null to disable read concern.
 */
Mongo.prototype.setReadConcern = function(level) {
    if (!level) {
        this._readConcernLevel = undefined;
    } else if (level === "local" || level === "majority") {
        this._readConcernLevel = level;
    } else {
        throw Error("Invalid read concern.");
    }
};

/**
 * Gets the read concern.
 */
Mongo.prototype.getReadConcern = function() {
    return this._readConcernLevel;
};

connect = function(url, user, pass) {
    if (url instanceof MongoURI) {
        user = url.user;
        pass = url.password;
        url = url.uri;
    }
    if (user && !pass)
        throw Error("you specified a user and not a password.  " +
                    "either you need a password, or you're using the old connect api");

    // Validate connection string "url" as "hostName:portNumber/databaseName"
    //                                  or "hostName/databaseName"
    //                                  or "databaseName"
    //                                  or full mongo uri.
    var urlType = typeof url;
    if (urlType == "undefined") {
        throw Error("Missing connection string");
    }
    if (urlType != "string") {
        throw Error("Incorrect type \"" + urlType + "\" for connection string \"" + tojson(url) +
                    "\"");
    }
    url = url.trim();
    if (0 == url.length) {
        throw Error("Empty connection string");
    }

    if (!url.startsWith("mongodb://") && !url.startsWith("mongodb+srv://")) {
        const colon = url.lastIndexOf(":");
        const slash = url.lastIndexOf("/");
        if (url.split("/").length > 1) {
            url = url.substring(0, slash).replace(/\//g, "%2F") + url.substring(slash);
        }
        if (slash == 0) {
            throw Error("Failed to parse mongodb:// URL: " + url);
        }
        if (slash == -1 && colon == -1) {
            url = "mongodb://127.0.0.1:27017/" + url;
        } else if (slash != -1) {
            url = "mongodb://" + url;
        }
    }

    chatty("connecting to: " + url);
    var m = new Mongo(url);
    var db = m.getDB(m.defaultDB);

    if (user && pass) {
        if (!db.auth(user, pass)) {
            throw Error("couldn't login");
        }
    }

    // Check server version
    var serverVersion = db.version();
    chatty("MongoDB server version: " + serverVersion);

    var shellVersion = version();
    if (serverVersion.slice(0, 3) != shellVersion.slice(0, 3)) {
        chatty("WARNING: shell and server versions do not match");
    }

    // Optionally display free monitoring message.
    const freemonStatus = db.adminCommand({getFreeMonitoringStatus: 1});
    if (freemonStatus.ok) {
        if (freemonStatus.userReminder) {
            chatty(freemonStatus.userReminder);
        }
    }

    return db;
};

/** deprecated, use writeMode below
 *
 */
Mongo.prototype.useWriteCommands = function() {
    return (this.writeMode() != "legacy");
};

Mongo.prototype.forceWriteMode = function(mode) {
    this._writeMode = mode;
};

Mongo.prototype.hasWriteCommands = function() {
    var hasWriteCommands = (this.getMinWireVersion() <= 2 && 2 <= this.getMaxWireVersion());
    return hasWriteCommands;
};

Mongo.prototype.hasExplainCommand = function() {
    var hasExplain = (this.getMinWireVersion() <= 3 && 3 <= this.getMaxWireVersion());
    return hasExplain;
};

/**
 * {String} Returns the current mode set. Will be commands/legacy/compatibility
 *
 * Sends isMaster to determine if the connection is capable of using bulk write operations, and
 * caches the result.
 */

Mongo.prototype.writeMode = function() {

    if ('_writeMode' in this) {
        return this._writeMode;
    }

    // get default from shell params
    if (_writeMode)
        this._writeMode = _writeMode();

    // can't use "commands" mode unless server version is good.
    if (this.hasWriteCommands()) {
        // good with whatever is already set
    } else if (this._writeMode == "commands") {
        this._writeMode = "compatibility";
    }

    return this._writeMode;
};

/**
 * Returns true if the shell is configured to use find/getMore commands rather than the C++ client.
 *
 * Currently, the C++ client will always use OP_QUERY find and OP_GET_MORE.
 */
Mongo.prototype.useReadCommands = function() {
    return (this.readMode() === "commands");
};

/**
 * For testing, forces the shell to use the readMode specified in 'mode'. Must be either "commands"
 * (use the find/getMore commands), "legacy" (use legacy OP_QUERY/OP_GET_MORE wire protocol reads),
 * or "compatibility" (auto-detect mode based on wire version).
 */
Mongo.prototype.forceReadMode = function(mode) {
    if (mode !== "commands" && mode !== "compatibility" && mode !== "legacy") {
        throw new Error("Mode must be one of {commands, compatibility, legacy}, but got: " + mode);
    }

    this._readMode = mode;
};

/**
 * Get the readMode string (either "commands" for find/getMore commands, "legacy" for OP_QUERY find
 * and OP_GET_MORE, or "compatibility" for detecting based on wire version).
 */
Mongo.prototype.readMode = function() {
    // Get the readMode from the shell params if we don't have one yet.
    if (typeof _readMode === "function" && !this.hasOwnProperty("_readMode")) {
        this._readMode = _readMode();
    }

    if (this.hasOwnProperty("_readMode") && this._readMode !== "compatibility") {
        // We already have determined our read mode. Just return it.
        return this._readMode;
    } else {
        // We're in compatibility mode. Determine whether the server supports the find/getMore
        // commands. If it does, use commands mode. If not, degrade to legacy mode.
        try {
            var hasReadCommands = (this.getMinWireVersion() <= 4 && 4 <= this.getMaxWireVersion());
            if (hasReadCommands) {
                this._readMode = "commands";
            } else {
                this._readMode = "legacy";
            }
        } catch (e) {
            // We failed trying to determine whether the remote node supports the find/getMore
            // commands. In this case, we keep _readMode as "compatibility" and the shell should
            // issue legacy reads. Next time around we will issue another isMaster to try to
            // determine the readMode decisively.
        }
    }

    return this._readMode;
};

//
// Write Concern can be set at the connection level, and is used for all write operations unless
// overridden at the collection level.
//

Mongo.prototype.setWriteConcern = function(wc) {
    if (wc instanceof WriteConcern) {
        this._writeConcern = wc;
    } else {
        this._writeConcern = new WriteConcern(wc);
    }
};

Mongo.prototype.getWriteConcern = function() {
    return this._writeConcern;
};

Mongo.prototype.unsetWriteConcern = function() {
    delete this._writeConcern;
};

Mongo.prototype.advanceClusterTime = function(newTime) {
    if (!newTime.hasOwnProperty("clusterTime")) {
        throw new Error("missing clusterTime field in setClusterTime argument");
    }

    if (typeof this._clusterTime === "object" && this._clusterTime !== null) {
        this._clusterTime =
            (bsonWoCompare({_: this._clusterTime.clusterTime}, {_: newTime.clusterTime}) >= 0)
            ? this._clusterTime
            : newTime;
    } else {
        this._clusterTime = newTime;
    }
};

Mongo.prototype.resetClusterTime_forTesting = function() {
    delete this._clusterTime;
};

Mongo.prototype.getClusterTime = function() {
    return this._clusterTime;
};

Mongo.prototype.startSession = function startSession(options = {}) {
    // Set retryWrites if not already set on options.
    if (!options.hasOwnProperty("retryWrites") && this.hasOwnProperty("_retryWrites")) {
        options.retryWrites = this._retryWrites;
    }
    return new DriverSession(this, options);
};

Mongo.prototype._getDefaultSession = function getDefaultSession() {
    // We implicitly associate a Mongo connection object with a DriverSession so that tests which
    // call DB.prototype.getMongo() and then Mongo.prototype.getDB() to get a different DB instance
    // are still causally consistent.
    if (!this.hasOwnProperty("_defaultSession")) {
        this._defaultSession = new _DummyDriverSession(this);
    }
    return this._defaultSession;
};

Mongo.prototype.isCausalConsistency = function isCausalConsistency() {
    if (!this.hasOwnProperty("_causalConsistency")) {
        this._causalConsistency = false;
    }
    return this._causalConsistency;
};

Mongo.prototype.setCausalConsistency = function setCausalConsistency(causalConsistency = true) {
    this._causalConsistency = causalConsistency;
};

Mongo.prototype.waitForClusterTime = function waitForClusterTime(maxRetries = 10) {
    let isFirstTime = true;
    let count = 0;
    while (count < maxRetries) {
        if (typeof this._clusterTime === "object" && this._clusterTime !== null) {
            if (this._clusterTime.hasOwnProperty("signature") &&
                this._clusterTime.signature.keyId > 0) {
                return;
            }
        }
        if (isFirstTime) {
            isFirstTime = false;
        } else {
            sleep(500);
        }
        count++;
        this.adminCommand({"ping": 1});
    }
    throw new Error("failed waiting for non default clusterTime");
};

Mongo.prototype.watch = function(pipeline, options) {
    pipeline = pipeline || [];
    options = options || {};
    assert(pipeline instanceof Array, "'pipeline' argument must be an array");
    assert(options instanceof Object, "'options' argument must be an object");

    let changeStreamStage = {
        allChangesForCluster: true,
        fullDocument: options.fullDocument || "default"
    };
    delete options.allChangesForCluster;
    delete options.fullDocument;

    if (options.hasOwnProperty("resumeAfter")) {
        changeStreamStage.resumeAfter = options.resumeAfter;
        delete options.resumeAfter;
    }

    if (options.hasOwnProperty("startAtOperationTime")) {
        changeStreamStage.startAtOperationTime = options.startAtOperationTime;
        delete options.startAtOperationTime;
    }

    pipeline.unshift({$changeStream: changeStreamStage});
    return this.getDB("admin")._runAggregate({aggregate: 1, pipeline: pipeline}, options);
};
