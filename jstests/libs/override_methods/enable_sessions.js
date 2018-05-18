/**
 * Enables sessions on the db object
 */
(function() {
    "use strict";

    load("jstests/libs/override_methods/override_helpers.js");

    const getDBOriginal = Mongo.prototype.getDB;

    const sessionMap = new WeakMap();
    const sessionOptions = TestData.sessionOptions;

    // Override the runCommand to check for any command obj that does not contain a logical session
    // and throw an error.
    function runCommandWithLsidCheck(conn, dbName, cmdName, cmdObj, func, makeFuncArgs) {
        if (jsTest.options().disableEnableSessions) {
            return func.apply(conn, makeFuncArgs(cmdObj));
        }

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
        return func.apply(conn, makeFuncArgs(cmdObj));
    }

    // Override the getDB to return a db object with the correct driverSession. We use a WeakMap
    // to cache the session for each connection instance so we can retrieve the same session on
    // subsequent calls to getDB.
    Mongo.prototype.getDB = function(dbName) {
        if (jsTest.options().disableEnableSessions) {
            return getDBOriginal.apply(this, arguments);
        }

        if (!sessionMap.has(this)) {
            const session = this.startSession(sessionOptions);
            // Override the endSession function to be a no-op so jstestfuzz doesn't accidentally
            // end the session.
            session.endSession = Function.prototype;
            sessionMap.set(this, session);
        }

        const db = getDBOriginal.apply(this, arguments);
        db._session = sessionMap.get(this);
        return db;
    };

    // Override the global `db` object to be part of a session.
    db = db.getMongo().getDB(db.getName());

    OverrideHelpers.prependOverrideInParallelShell(
        "jstests/libs/override_methods/enable_sessions.js");
    OverrideHelpers.overrideRunCommand(runCommandWithLsidCheck);

})();
