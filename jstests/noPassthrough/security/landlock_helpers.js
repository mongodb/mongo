/**
 * Returns `absPath` expressed relative to the shell's current working directory.
 * e.g. cwd=/a/b/c/d, input=/a/b/x/y, returns ../../x/y
 * @param {string} absPath - The absolute path to make relative.
 * @returns {string} The relative path.
 */
export function pathRelativeToStartupCwd(absPath) {
    const kCwd = pwd();
    const from = kCwd.split("/").filter((p) => p !== "");
    const to = absPath.split("/").filter((p) => p !== "");
    let shared = 0;
    while (shared < from.length && shared < to.length && from[shared] === to[shared]) {
        shared++;
    }
    const rel = Array(from.length - shared)
        .fill("..")
        .concat(to.slice(shared))
        .join("/");
    return rel === "" ? "." : rel;
}

/**
 * Returns `absPath` relative to "/", which is the cwd the mongodb daemon runs with after
 * a fork.
 * @param {string} absPath - The absolute path to make relative.
 * @returns {string} The relative path.
 */
export function pathRelativeToDaemonCwd(absPath) {
    return absPath.substring(1);
}

/**
 * Returns a path to a scratch directory under /var/tmp, whose root-relative name ("var/tmp/...")
 * a forked daemon can open.
 * /var/tmp is chosen as the base directory because access to it is not included in the built-in
 * Landlock filesystem rules.
 * @param {string} suffix - used to build a unique scratch directory attributable to 'suffix'
 * @returns {string} the path to the scratch directory
 */
export function getLandlockRootRelativeScratchDirectory(suffix) {
    return `/var/tmp/${jsTestName()}-${suffix}-${new Date().getTime()}`;
}

/**
 * Starts a --fork'd mongod from a raw options object. MongoRunner's
 * runMongod() is bypassed deliberately.
 *
 * The parent of a forked mongod exits once the daemon is serving, so the launch returns rather
 * than blocking; a non-zero exit means the daemon never got there.
 * @param {Object} opts - options passed to MongoRunner
 * @returns {Object} - object containing {conn, pid}, conn is the Mongo connection
 * and pid is the forked daemon's process ID.
 */
export function startForkedMongod(opts) {
    clearRawMongoProgramOutput();
    const args = MongoRunner.arrOptions("mongod", opts);
    jsTest.log.info("Launching forked mongod", {args});
    assert.eq(0, runMongoProgram(...args), "forked mongod's parent process failed");

    const match = rawMongoProgramOutput(".*").match(/forked process: (\d+)/);
    assert.neq(null, match, "could not find the forked daemon's pid in the launcher output");
    return {conn: new Mongo(`127.0.0.1:${opts.port}`), pid: parseInt(match[1], 10)};
}

/**
 * Shuts down a forked daemon. It is not a child of this shell, so MongoRunner cannot wait on it:
 * shut it down over the connection and only fall back to a signal if that does not take.
 * @param {Object} conn - the Mongo connection to the daemon
 * @param {daemonPid} - the daemon PID
 */
export function stopForkedMongod(conn, daemonPid) {
    if (conn) {
        try {
            conn.getDB("admin").auth("admin", "pwd");
            conn.adminCommand({shutdown: 1});
        } catch (e) {
            // The server closes the connection while shutting down, so this always throws.
            jsTest.log.info("connection dropped during shutdown, as expected", {
                error: e.toString(),
            });
        }
    }
    if (daemonPid) {
        assert.soon(
            () => runNonMongoProgram("kill", "-0", String(daemonPid)) !== 0,
            "forked daemon did not exit after shutdown",
            60 * 1000,
        );
    }
}

/**
 * Asserts the server reports the landlock status in serverStatus and
 * the landlock mode matches the expected mode.
 * @param {Object} conn - server connection (authenticated & authorized to run serverStatus)
 * @param {string} mode - the expected landlock mode
 * @returns {Object} - the landlock section of the retrieved serverStatus
 */
export function assertLandlockMode(conn, mode) {
    const res = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
    const landlock = res.landlock;
    assert.neq(undefined, landlock, `missing landlock section in serverStatus`);
    assert.eq(mode, landlock.mode, `wrong landlock mode reported in serverStatus`);
    if (landlock.mode !== "disabled") {
        assert.eq(
            landlock.active,
            landlock.abiVersion > 0,
            `landlock active state does not agree with abiVersion value`,
        );
    } else {
        assert.eq(false, landlock.active, `landlock.active is true even if landlock.mode=disabled`);
    }
    return landlock;
}
