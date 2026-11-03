import {after, before} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

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
    return `/var/tmp/${jsTestName().slice(0, 20)}-${suffix}-${UUID().hex().slice(0, 16)}`;
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
    assert.eq(mode, landlock.mode, `wrong landlock mode reported in serverStatus`, {landlock});

    assert.eq("boolean", typeof landlock.active, `invalid landlock.active type`, {landlock});
    assert.eq("number", typeof landlock.policyVersion, `invalid landlock.policyVersion type`, {
        landlock,
    });
    assert.eq(
        "number",
        typeof landlock.latestPolicyVersion,
        `invalid landlock.latestPolicyVersion type`,
        {landlock},
    );
    assert.eq("number", typeof landlock.abiVersion, `invalid landlock.abiVersion type`, {landlock});
    assert.gte(landlock.abiVersion, 0, `invalid landlock.abiVersion value`, {landlock});

    if (landlock.mode !== "disabled") {
        assert.eq(
            landlock.active,
            landlock.abiVersion > 0,
            `landlock active state does not agree with abiVersion value`,
        );
    } else {
        assert.eq(false, landlock.active, `landlock.active is true even if landlock.mode=disabled`);
    }

    if (landlock.active) {
        assert(Array.isArray(landlock.handledAccessRights.fs), `bad handledAccessRights type`, {
            landlock,
        });
        assert.gt(landlock.handledAccessRights.fs.length, 0);
        assert(Array.isArray(landlock.degradedAccessRights.fs), `bad degradedAccessRights type`, {
            landlock,
        });
    } else {
        assert(!landlock.hasOwnProperty("handledAccessRights"));
        assert(!landlock.hasOwnProperty("degradedAccessRights"));
    }

    return landlock;
}

const kSkipStopChecks = {skipValidation: true, skipCheckDBHashes: true};

/**
 * Launches and stops the two kinds of server process that run the Landlock sandbox, mongod and
 * mongos, without a ShardingTest.
 *
 * A mongos needs a config server to start against, not a whole cluster, so a bare one-node config
 * replica set backs the fixture and each mongos is launched beside it. Use
 * useLandlockProcessFixture() to have a describe() block run it.
 *
 * `processes` maps a process name to {start(opts), stop(conn)}, where `start` takes the options
 * MongoRunner would and returns the connection (null or a throw when the process refuses to
 * start), and `stop` stops it without any consistency checks.
 */
export class LandlockProcessFixture {
    constructor() {
        this._configRS = null;
        this.processes = {
            mongod: {
                start: (opts) => MongoRunner.runMongod(opts),
                stop: (conn) => MongoRunner.stopMongod(conn, undefined, kSkipStopChecks),
            },
            mongos: {
                // A mongos waits out a 15 second quiesce period on a signaled shutdown unless
                // told otherwise; ShardingTest zeroes it for its own routers, not for these.
                start: (opts) =>
                    MongoRunner.runMongos({
                        configdb: this._configRS.getURL(),
                        setParameter: {mongosShutdownTimeoutMillisForSignaledShutdown: 0},
                        ...opts,
                    }),
                stop: (conn) => MongoRunner.stopMongos(conn, undefined, kSkipStopChecks),
            },
        };
    }

    setUp() {
        this._configRS = new ReplSetTest({nodes: 1});
        this._configRS.startSet({configsvr: ""});
        this._configRS.initiate();
    }

    tearDown() {
        if (this._configRS) {
            this._configRS.stopSet(undefined, false, kSkipStopChecks);
            this._configRS = null;
        }
    }
}

/**
 * Registers before/after hooks on the enclosing describe() that set up and tear down a
 * LandlockProcessFixture, and returns it. Call it directly inside the describe() callback.
 */
export function useLandlockProcessFixture() {
    const fixture = new LandlockProcessFixture();
    before(() => fixture.setUp());
    after(() => fixture.tearDown());
    return fixture;
}

// The node's own log lines with this id.
export function logEntriesWithId(node, id) {
    return cat(node.fullOptions.logFile)
        .split("\n")
        .filter((line) => line.includes(`"id":${id}`))
        .map((line) => JSON.parse(line));
}

// Starts a node, runs `fn` on it, and stops it even when `fn` fails.
export function withProcess({start, stop}, opts, fn) {
    const conn = start(opts);
    assert.neq(null, conn, "process failed to start", {opts});
    try {
        return fn(conn);
    } finally {
        stop(conn);
    }
}

// Probes landlock support on the current platform using the mongod binary.
export function probeLandlockSupport() {
    const conn = MongoRunner.runMongod({landlockMode: "bestEffort"});
    assert.neq(null, conn, "mongod for probing landlock support failed to start");
    const landlock = assertLandlockMode(conn, "bestEffort");
    MongoRunner.stopMongod(conn, undefined, {skipValidation: true});
    return {latestPolicyVersion: landlock.latestPolicyVersion, active: landlock.active};
}
