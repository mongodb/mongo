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

Mongo.prototype.getDBs = function() {
    var res = this.getDB("admin").runCommand({"listDatabases": 1});
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
Mongo.prototype.getLogComponents = function() {
    var res = this.adminCommand({getParameter: 1, logComponentVerbosity: 1});
    if (!res.ok)
        throw _getErrorWithCode(res, "getLogComponents failed:" + tojson(res));
    return res.logComponentVerbosity;
};

/**
 * Accepts optional second argument "component",
 * string of form "storage.journaling"
 */
Mongo.prototype.setLogLevel = function(logLevel, component) {
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
    var res = this.adminCommand({setParameter: 1, logComponentVerbosity: vDoc});
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

connect = function(url, user, pass) {
    if (user && !pass)
        throw Error("you specified a user and not a password.  " +
                    "either you need a password, or you're using the old connect api");

    // Validate connection string "url" as "hostName:portNumber/databaseName"
    //                                  or "hostName/databaseName"
    //                                  or "databaseName"
    // hostName may be an IPv6 address (with colons), in which case ":portNumber" is required
    //
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
    if (!url.startsWith("mongodb://")) {
        var colon = url.lastIndexOf(":");
        var slash = url.lastIndexOf("/");
        if (0 == colon || 0 == slash) {
            throw Error("Missing host name in connection string \"" + url + "\"");
        }
        if (colon == slash - 1 || colon == url.length - 1) {
            throw Error("Missing port number in connection string \"" + url + "\"");
        }
        if (colon != -1 && colon < slash) {
            var portNumber = url.substring(colon + 1, slash);
            if (portNumber.length > 5 || !/^\d*$/.test(portNumber) ||
                parseInt(portNumber) > 65535) {
                throw Error("Invalid port number \"" + portNumber + "\" in connection string \"" +
                            url + "\"");
            }
        }
        if (slash == url.length - 1) {
            throw Error("Missing database name in connection string \"" + url + "\"");
        }
    }

    var db;
    if (url.startsWith("mongodb://")) {
        chatty("connecting to: " + url);
        db = new Mongo(url);
        if (db.defaultDB.length == 0) {
            db.defaultDB = "test";
        }
        db = db.getDB(db.defaultDB);
    } else if (slash == -1) {
        chatty("connecting to: 127.0.0.1:27017/" + url);
        db = new Mongo().getDB(url);
    } else {
        var hostPart = url.substring(0, slash);
        var dbPart = url.substring(slash + 1);
        chatty("connecting to: " + hostPart + "/" + dbPart);
        db = new Mongo(hostPart).getDB(dbPart);
    }

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
        print("Cannot use commands write mode, degrading to compatibility mode");
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
            // TODO SERVER-23219: DBCommandCursor doesn't route getMore and killCursors operations
            // to the server that the cursor was originally established on. As a workaround, we make
            // replica set connections use 'legacy' read mode because the underlying DBClientCursor
            // will correctly route operations to the original server.
            if (hasReadCommands && !this.isReplicaSetConnection()) {
                this._readMode = "commands";
            } else {
                print("Cannot use 'commands' readMode, degrading to 'legacy' mode");
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
