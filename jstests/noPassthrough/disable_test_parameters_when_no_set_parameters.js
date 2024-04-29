/**
 * Tests that when setParameter is not included in the startup options, we still disable test-only
 * parameters. This was a bug fixed in SERVER-89495.
 *
 * Incompatible with concurrency because we modify MongoRunner._startWithArgs.
 * @tags: [incompatible_with_concurrency_simultaneous]
 */
const originalStartWithArgs = MongoRunner._startWithArgs;

MongoRunner._startWithArgs = function(argArray, env, waitForConnect) {
    // Remove all setParameters by iteratively matching the pattern ["--setParameter", "name=value"]
    // and deleting both entries from the arg array.
    let idx = argArray.indexOf("--setParameter");
    while (idx !== -1) {
        argArray.splice(idx, 2);
        idx = argArray.indexOf("--setParameter");
    }

    // Here and below is equivalent to the original code, minus a case that we never hit.
    const port = MongoRunner.parsePort.apply(null, argArray);
    let pid = -1;
    if (env === undefined) {
        pid = _startMongoProgram.apply(null, argArray);
    } else {
        pid = _startMongoProgram({args: argArray, env: env});
    }

    if (!waitForConnect) {
        print("Skip waiting to connect to node with pid=" + pid + ", port=" + port);
        return {
            pid: pid,
            port: port,
        };
    }

    return MongoRunner.awaitConnection({pid, port});
};

const conn = MongoRunner.runMongod();
const db = conn.getDB('admin');

// We shouldn't be able to fetch test-only server or cluster parameters.
assert.commandFailedWithCode(db.runCommand({getParameter: 1, requireApiVersion: 1}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(db.runCommand({getClusterParameter: "cwspTestNeedsLatestFCV"}),
                             ErrorCodes.BadValue);

MongoRunner.stopMongod(conn);
MongoRunner._startWithArgs = originalStartWithArgs;
