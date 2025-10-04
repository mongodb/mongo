/**
 * Enables sessions on the db object
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const getDBOriginal = Mongo.prototype.getDB;

const sessionMap = new WeakMap();
const sessionOptions = TestData.sessionOptions;

// Override the runCommand to check for any command obj that does not contain a logical session
// and throw an error.
function runCommandWithLsidCheck(conn, dbName, cmdName, cmdObj, func, makeFuncArgs) {
    if (jsTest.options().disableEnableSessions) {
        return func.apply(conn, makeFuncArgs(cmdObj));
    }

    if (!cmdObj.hasOwnProperty("lsid")) {
        // Throw an error for requests not using sessions.
        throw new Error("command object does not have session id: " + tojson(cmdObj));
    }
    return func.apply(conn, makeFuncArgs(cmdObj));
}

// Override the getDB to return a db object with the correct driverSession. We use a WeakMap
// to cache the session for each connection instance so we can retrieve the same session on
// subsequent calls to getDB.
Mongo.prototype.getDB = function (dbName) {
    if (jsTest.options().disableEnableSessions) {
        return getDBOriginal.apply(this, arguments);
    }

    if (
        sessionOptions &&
        sessionOptions.hasOwnProperty("maybeUseCausalConsistency") &&
        sessionOptions.maybeUseCausalConsistency === true
    ) {
        const causalConsistency = Math.random() < 0.5;
        sessionOptions.causalConsistency = causalConsistency;
        jsTestLog(`Sessions override setting causalConsistency=${causalConsistency} for db ${dbName}`);

        delete sessionOptions.maybeUseCausalConsistency;
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
globalThis.db = db.getMongo().getDB(db.getName());

OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/enable_sessions.js");
OverrideHelpers.overrideRunCommand(runCommandWithLsidCheck);
