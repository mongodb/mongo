/**
 * Allows passing arguments to the function executed by startParallelShell.
 */
const funWithArgs = (fn, ...args) =>
    "(" + fn.toString() + ")(" + args.map(x => tojson(x)).reduce((x, y) => x + ", " + y) + ")";

/**
 * Internal function used by _doAssertCommandInParallelShell().
 */
const _parallelShellRunCommand = function(dbName, cmdObj, expectedCode) {
    const expectedCodeStr =
        (expectedCode === undefined ? '' : '; expectedCode: ' + tojson(expectedCode));
    jsTestLog('Starting command in parallel shell - host: ' + db.getMongo().host +
              '; db: ' + dbName + '; command: ' + tojson(cmdObj) + expectedCodeStr);
    const testDB = db.getSiblingDB(dbName);
    const result = testDB.runCommand(cmdObj);
    jsTestLog('Finished running command in parallel shell - host: ' + db.getMongo().host +
              '; db: ' + dbName + '; command: ' + tojson(cmdObj) + expectedCodeStr +
              '; result: ' + tojson(result));
    if (expectedCode === undefined) {
        assert.commandWorked(result);
    } else {
        assert.commandFailedWithCode(result, expectedCode);
    }
};

/**
 * Internal function used by assertCommandWorkedInParallelShell() and
 * assertCommandFailedWithCodeInParallelShell().
 */
const _doAssertCommandInParallelShell = function(conn, db, cmdObj, expectedCode) {
    // Return joinable object to caller.
    return startParallelShell(
        funWithArgs(_parallelShellRunCommand, db.getName(), cmdObj, expectedCode), conn.port);
};

/**
 * Starts command in a parallel shell.
 * Provides similar behavior to assert.commandWorked().
 */
const assertCommandWorkedInParallelShell = function(conn, db, cmdObj) {
    return _doAssertCommandInParallelShell(conn, db, cmdObj);
};

/**
 * Starts command in a parallel shell.
 * Provides similar behavior to assert.commandFailedWithCode().
 */
const assertCommandFailedWithCodeInParallelShell = function(conn, db, cmdObj, expectedCode) {
    assert(expectedCode,
           'expected error code(s) must be provided to run command in parallel shell - host: ' +
               db.getMongo().host + '; db: ' + db.getName() + '; command: ' + tojson(cmdObj));
    return _doAssertCommandInParallelShell(conn, db, cmdObj, expectedCode);
};
