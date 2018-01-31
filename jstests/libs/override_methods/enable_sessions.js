/**
 * Enables sessions on the db object
 */
(function() {
    "use strict";

    load("jstests/libs/override_methods/override_helpers.js");

    var runCommandOriginal = Mongo.prototype.runCommand;
    var runCommandWithMetadataOriginal = Mongo.prototype.runCommandWithMetadata;
    var getDBOriginal = Mongo.prototype.getDB;
    var sessionMap = new WeakMap();

    let sessionOptions = {};
    if (typeof TestData !== "undefined" && TestData.hasOwnProperty("sessionOptions")) {
        sessionOptions = TestData.sessionOptions;
    }

    const driverSession = startSession(db.getMongo());
    db = driverSession.getDatabase(db.getName());
    sessionMap.set(db.getMongo(), driverSession);

    OverrideHelpers.prependOverrideInParallelShell(
        "jstests/libs/override_methods/enable_sessions.js");

    function startSession(conn) {
        const driverSession = conn.startSession(sessionOptions);
        // Override the endSession function to be a no-op so fuzzer doesn't accidentally end the
        // session.
        driverSession.endSession = Function.prototype;
        return driverSession;
    }

    // Override the runCommand to check for any command obj that does not contain a logical session
    // and throw an error.
    function runCommandWithLsidCheck(conn, dbName, cmdObj, func, funcArgs) {
        if (jsTest.options().disableEnableSessions) {
            return func.apply(conn, funcArgs);
        }

        const cmdName = Object.keys(cmdObj)[0];

        // If the command is in a wrapped form, then we look for the actual command object
        // inside the query/$query object.
        let cmdObjUnwrapped = cmdObj;
        if (cmdName === "query" || cmdName === "$query") {
            cmdObj[cmdName] = Object.assign({}, cmdObj[cmdName]);
            cmdObjUnwrapped = cmdObj[cmdName];
        }

        if (!cmdObjUnwrapped.hasOwnProperty("lsid")) {
            // TODO: SERVER-30848 fixes getMore requests to use a session in the mongo shell.
            // Until that happens, we bypass throwing an error for getMore and only throw an error
            // for other requests not using sessions.
            if (cmdName !== "getMore") {
                throw new Error("command object does not have session id: " + tojson(cmdObj));
            }
        }
        return func.apply(conn, funcArgs);
    }

    Mongo.prototype.runCommand = function(dbName, commandObj, options) {
        return runCommandWithLsidCheck(this, dbName, commandObj, runCommandOriginal, arguments);
    };

    Mongo.prototype.runCommandWithMetadata = function(dbName, metadata, commandObj) {
        return runCommandWithLsidCheck(
            this, dbName, commandObj, runCommandWithMetadataOriginal, arguments);
    };

    // Override the getDB to return a db object with the correct driverSession. We use a WeakMap
    // to cache the session for each connection instance so we can retrieve the same session on
    // subsequent calls to getDB.
    Mongo.prototype.getDB = function(dbName) {
        if (jsTest.options().disableEnableSessions) {
            return getDBOriginal.apply(this, arguments);
        }

        if (!sessionMap.has(this)) {
            const session = startSession(this);
            sessionMap.set(this, session);
        }

        const db = getDBOriginal.apply(this, arguments);
        db._session = sessionMap.get(this);
        return db;
    };

})();
