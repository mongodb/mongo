// mongo.js

// Defined in mongojs.cpp

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

Mongo.prototype.setSlaveOk = function(value) {
    print(
        "WARNING: setSlaveOk() is deprecated and may be removed in the next major release. Please use setSecondaryOk() instead.");
    this.setSecondaryOk(value);
};

Mongo.prototype.getSlaveOk = function() {
    print(
        "WARNING: getSlaveOk() is deprecated and may be removed in the next major release. Please use getSecondaryOk() instead.");
    return this.getSecondaryOk();
};

Mongo.prototype.setSecondaryOk = function(value = true) {
    this.secondaryOk = value;
};

Mongo.prototype.getSecondaryOk = function() {
    return this.secondaryOk || false;
};

Mongo.prototype.getDB = function(name) {
    if ((jsTest.options().keyFile) &&
        ((typeof this.authenticated == 'undefined') || !this.authenticated)) {
        jsTest.authenticate(this);
    }
    // There is a weird issue where typeof(db._name) !== "string" when the db name
    // is created from objects returned from native C++ methods.
    // This hack ensures that the db._name is always a string.
    if (typeof (name) === "object") {
        name = name.toString();
    }
    return new DB(this, name);
};

Mongo.prototype._getDatabaseNamesFromPrivileges = function() {
    'use strict';

    const ret = this.adminCommand({connectionStatus: 1, showPrivileges: 1});
    if (!ret.ok) {
        throw _getErrorWithCode(ret, "Failed to acquire database information from privileges");
    }

    const privileges = (ret.authInfo || {}).authenticatedUserPrivileges;
    if (privileges === undefined) {
        return [];
    }

    return privileges
        .filter(function(priv) {
            // Find all named databases in priv list.
            return ((priv.resource || {}).db || '').length > 0;
        })
        .map(function(priv) {
            // Return just the names.
            return priv.resource.db;
        })
        .filter(function(db, idx, arr) {
            // Make sure the list is unique
            return arr.indexOf(db) === idx;
        })
        .sort();
};

Mongo.prototype.getDBs = function(driverSession = this._getDefaultSession(),
                                  filter = undefined,
                                  nameOnly = undefined,
                                  authorizedDatabases = undefined) {
    return function(driverSession, filter, nameOnly, authorizedDatabases) {
        'use strict';

        const multitenancyRes = this.adminCommand({getParameter: 1, multitenancySupport: 1});
        const multitenancy = multitenancyRes.ok && multitenancyRes["multitenancySupport"];

        // Calling listDatases is only valid if we have a security token in multitenancy mode.
        // Otherwise we call listDatabasesForAllTenants which list db.name and db.tenantId
        // separately. The result never has a tenant prefix.
        let cmdObj = multitenancy && !this._securityToken ? {listDatabasesForAllTenants: 1}
                                                          : {listDatabases: 1};
        if (filter !== undefined) {
            cmdObj.filter = filter;
        }
        if (nameOnly !== undefined) {
            cmdObj.nameOnly = nameOnly;
        }
        if (authorizedDatabases !== undefined) {
            cmdObj.authorizedDatabases = authorizedDatabases;
        }

        if (driverSession._isExplicit || !jsTest.options().disableImplicitSessions) {
            cmdObj = driverSession._serverSession.injectSessionId(cmdObj);
        }

        const res = this.adminCommand(cmdObj);
        if (!res.ok) {
            // If "Unauthorized" was returned by the back end and we haven't explicitly
            // asked for anything difficult to provide from userspace, then we can
            // fallback on inspecting the user's permissions.
            // This means that:
            //   * filter must be undefined, as reimplementing that logic is out of scope.
            //   * nameOnly must not be false as we can't infer size information.
            //   * authorizedDatabases must not be false as those are the only DBs we can infer.
            // Note that if the above are valid and we get Unauthorized, that also means
            // that we MUST be talking to a pre-4.0 mongod.
            //
            // Like the server response mode, this path will return a simple list of
            // names if nameOnly is specified as true.
            // If nameOnly is undefined, we come as close as we can to what the
            // server would return by supplying the databases key of the returned
            // object.  Other information is unavailable.
            if ((res.code === ErrorCodes.Unauthorized) && (filter === undefined) &&
                (nameOnly !== false) && (authorizedDatabases !== false)) {
                const names = this._getDatabaseNamesFromPrivileges();
                if (nameOnly === true) {
                    return names;
                } else {
                    return {
                        databases: names.map(function(x) {
                            return {name: x};
                        }),
                    };
                }
            }
            throw _getErrorWithCode(res, "listDatabases failed:" + tojson(res));
        }

        if (nameOnly) {
            return res.databases.map(function(db) {
                return db.name;
            });
        }

        return res;
    }.call(this, driverSession, filter, nameOnly, authorizedDatabases);
};

Mongo.prototype.adminCommand = function(cmd) {
    return this.getDB("admin").runCommand(cmd);
};

Mongo.prototype._setSecurityToken = function(token) {
    this._securityToken = token;
};

Mongo.prototype.runCommand = function(dbname, cmd, options) {
    return this._runCommandImpl(dbname, cmd, options, this._securityToken);
};

/**
 * Returns all log components and current verbosity values
 */
Mongo.prototype.getLogComponents = function(driverSession = this._getDefaultSession()) {
    var cmdObj = {getParameter: 1, logComponentVerbosity: 1};
    if (driverSession._isExplicit || !jsTest.options().disableImplicitSessions) {
        cmdObj = driverSession._serverSession.injectSessionId(cmdObj);
    }

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
    let componentNames = [];
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
    if (driverSession._isExplicit || !jsTest.options().disableImplicitSessions) {
        cmdObj = driverSession._serverSession.injectSessionId(cmdObj);
    }

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
 * @param hedgeOptions {<Object>} optional. The hedge options of the form {enabled: <bool>}.
 */
Mongo.prototype.setReadPref = function(mode, tagSet, hedgeOptions) {
    if (this._readPrefMode === "primary") {
        if ((typeof (tagSet) !== "undefined") && (Object.keys(tagSet).length > 0)) {
            // we allow empty arrays/objects or no tagSet for compatibility reasons
            throw Error("Cannot supply tagSet with readPref mode \"primary\"");
        }
        if ((typeof (hedgeOptions) === "object") && hedgeOptions.enabled) {
            throw Error("Cannot enable hedging with readPref mode \"primary\"");
        }
    }

    this._setReadPrefUnsafe(mode, tagSet, hedgeOptions);
};

// Set readPref without validating. Exposed so we can test the server's readPref validation.
Mongo.prototype._setReadPrefUnsafe = function(mode, tagSet, hedgeOptions) {
    this._readPrefMode = mode;
    this._readPrefTagSet = tagSet;
    this._readPrefHedgeOptions = hedgeOptions;
};

Mongo.prototype.getReadPrefMode = function() {
    return this._readPrefMode;
};

Mongo.prototype.getReadPrefTagSet = function() {
    return this._readPrefTagSet;
};

Mongo.prototype.getReadPrefHedgeOptions = function() {
    return this._readPrefHedgeOptions;
};

// Returns a readPreference object of the type expected by mongos.
Mongo.prototype.getReadPref = function() {
    let obj = {};

    const mode = this.getReadPrefMode();
    if (typeof (mode) === "string") {
        obj.mode = mode;
    } else {
        return null;
    }

    // Server Selection Spec: - if readPref mode is "primary" then the tags field MUST
    // be absent. Ensured by setReadPref.
    const tagSet = this.getReadPrefTagSet();
    if (Array.isArray(tagSet)) {
        obj.tags = tagSet;
    }

    // Hedged Reads Spec: - if readPref mode is "primary" then the hegde.enabled MUST
    // be false. Ensured by setReadPref.
    const hedgeOptions = this.getReadPrefHedgeOptions();
    if (typeof (hedgeOptions) === "object") {
        obj.hedge = hedgeOptions;
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

globalThis.connect = function(url, user, pass, apiParameters) {
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

    var atPos = url.indexOf("@");
    var protocolPos = url.indexOf("://");
    var safeURL = url;
    if (atPos != -1 && protocolPos != -1) {
        safeURL = url.substring(0, protocolPos + 3) + url.substring(atPos + 1);
    }
    chatty("connecting to: " + safeURL);
    try {
        var m = new Mongo(url, undefined /* encryptedDBClientCallback */, apiParameters);
    } catch (e) {
        var dest;
        if (url.indexOf(".query.mongodb.net") != -1) {
            dest = "MongoDB Atlas Data Lake";
        } else if (url.indexOf(".mongodb.net") != -1) {
            dest = "MongoDB Atlas cluster";
        }

        if (dest) {
            print(`\n\n*** You have failed to connect to a ${dest}. Please ensure` +
                  " that your IP allowlist allows connections from your network.\n\n");
        }

        throw e;
    }
    var db = m.getDB(m.defaultDB);

    if (user && pass) {
        if (!db.auth(user, pass)) {
            throw Error("couldn't login");
        }
    }

    if (_shouldUseImplicitSessions()) {
        chatty("Implicit session: " + db.getSession());
    }

    // Implicit sessions should not be used when opening a connection. In particular, the buildInfo
    // command is erroneously marked as requiring auth in MongoDB 3.6 and therefore fails if a
    // logical session id is included in the request.
    const originalTestData = TestData;
    TestData = Object.merge(originalTestData, {disableImplicitSessions: true});
    try {
        // Check server version
        var serverVersion = db.version();
        chatty("MongoDB server version: " + serverVersion);

        var shellVersion = version();
        if (serverVersion.slice(0, 3) != shellVersion.slice(0, 3)) {
            chatty("WARNING: shell and server versions do not match");
        }
    } finally {
        TestData = originalTestData;
    }

    return db;
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
    const newDriverSession = new DriverSession(this, options);

    // Only log this message if we are running a test
    if (typeof TestData === "object" && TestData.testName) {
        print("New session started with sessionID: " +
              tojsononeline(newDriverSession.getSessionId()) +
              " and options: " + tojsononeline(options));
    }

    return newDriverSession;
};

Mongo.prototype._getDefaultSession = function getDefaultSession() {
    // We implicitly associate a Mongo connection object with a real session so all requests include
    // a logical session id. These implicit sessions are intentionally not causally consistent. If
    // implicit sessions have been globally disabled, a dummy session is used instead of a real one.
    if (!this.hasOwnProperty("_defaultSession")) {
        if (_shouldUseImplicitSessions()) {
            try {
                this._defaultSession = this.startSession({causalConsistency: false});
            } catch (e) {
                if (e instanceof DriverSession.UnsupportedError) {
                    chatty("WARNING: No implicit session: " + e.message);
                    this._setDummyDefaultSession();
                } else {
                    print("ERROR: Implicit session failed: " + e.message);
                    throw (e);
                }
            }
        } else {
            this._setDummyDefaultSession();
        }
        this._defaultSession._isExplicit = false;
    }
    return this._defaultSession;
};

Mongo.prototype._setDummyDefaultSession = function setDummyDefaultSession() {
    this._defaultSession = new _DummyDriverSession(this);
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

/**
 * Given the options object for a 'watch' helper, determines which options apply to the change
 * stream stage, and which apply to the aggregate overall. Returns two objects: the change
 * stream stage specification and the options for the aggregate command, respectively.
 */
Mongo.prototype._extractChangeStreamOptions = function(options) {
    options = options || {};
    assert(options instanceof Object, "'options' argument must be an object");

    let changeStreamOptions = {fullDocument: options.fullDocument || "default"};
    delete options.fullDocument;

    if (options.hasOwnProperty("resumeAfter")) {
        changeStreamOptions.resumeAfter = options.resumeAfter;
        delete options.resumeAfter;
    }

    if (options.hasOwnProperty("startAfter")) {
        changeStreamOptions.startAfter = options.startAfter;
        delete options.startAfter;
    }

    if (options.hasOwnProperty("startAtOperationTime")) {
        changeStreamOptions.startAtOperationTime = options.startAtOperationTime;
        delete options.startAtOperationTime;
    }

    if (options.hasOwnProperty("fullDocumentBeforeChange")) {
        changeStreamOptions.fullDocumentBeforeChange = options.fullDocumentBeforeChange;
        delete options.fullDocumentBeforeChange;
    }

    if (options.hasOwnProperty("allChangesForCluster")) {
        changeStreamOptions.allChangesForCluster = options.allChangesForCluster;
        delete options.allChangesForCluster;
    }

    if (options.hasOwnProperty("allowToRunOnConfigDB")) {
        changeStreamOptions.allowToRunOnConfigDB = options.allowToRunOnConfigDB;
        delete options.allowToRunOnConfigDB;
    }

    if (options.hasOwnProperty("allowToRunOnSystemNS")) {
        changeStreamOptions.allowToRunOnSystemNS = options.allowToRunOnSystemNS;
        delete options.allowToRunOnSystemNS;
    }

    if (options.hasOwnProperty("showExpandedEvents")) {
        changeStreamOptions.showExpandedEvents = options.showExpandedEvents;
        delete options.showExpandedEvents;
    }

    if (options.hasOwnProperty("showSystemEvents")) {
        changeStreamOptions.showSystemEvents = options.showSystemEvents;
        delete options.showSystemEvents;
    }

    if (options.hasOwnProperty("showRawUpdateDescription")) {
        changeStreamOptions.showRawUpdateDescription = options.showRawUpdateDescription;
        delete options.showRawUpdateDescription;
    }

    return [{$changeStream: changeStreamOptions}, options];
};

Mongo.prototype.watch = function(pipeline, options) {
    pipeline = pipeline || [];
    assert(pipeline instanceof Array, "'pipeline' argument must be an array");

    const [changeStreamStage, aggOptions] = this._extractChangeStreamOptions(options);
    changeStreamStage.$changeStream.allChangesForCluster = true;
    pipeline.unshift(changeStreamStage);
    return this.getDB("admin")._runAggregate({aggregate: 1, pipeline: pipeline}, aggOptions);
};
