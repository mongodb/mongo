/**
 * Functions useful for testing startup and shutdown behavior.
 */

/**
 * Starts mongod with the given arguments, runs a hello command, and then terminates the process.
 */
export function startThenShutdownMongod(args = {}) {
    const mongod = MongoRunner.runMongod(args);
    assert.commandWorked(mongod.adminCommand({hello: 1}));
    MongoRunner.stopMongod(mongod);
}

/**
 * Starts mongos with the given config server and arguments, runs a hello command, and then
 * terminates the process.
 */
export function startThenShutdownMongos(configRS, args = {}) {
    const defaults = {configdb: configRS.getURL()};
    const mongos = MongoRunner.runMongos({...defaults, ...args});
    assert.commandWorked(mongos.adminCommand({hello: 1}));
    MongoRunner.stopMongos(mongos);
}

function parseExitCodeFromLogLine(line) {
    if (line.trim() == '') {
        return null;
    }

    const msg = JSON.parse(line);
    if (!msg.hasOwnProperty("msg")) {
        return null;
    }
    if (msg["msg"] != "Shutting down") {
        return null;
    }

    // This is the shutdown message.
    assert(msg.hasOwnProperty("attr"), "Shutdown log message missing attr field");
    assert(msg["attr"].hasOwnProperty("exitCode"), "Shutdown log message missing exitCode field");
    return msg["attr"]["exitCode"];
}

function getExitCode(line) {
    try {
        return parseExitCodeFromLogLine(line);
    } catch (e) {
        jsTestLog("Failed to parse JSON log line (" + e + "): " + line);
        throw e;
    }
}

function tryGetExitCode(line) {
    try {
        return parseExitCodeFromLogLine(line);
    } catch (e) {
        return null;
    }
}

/**
 * Scans through the given text, passing each line to the given callback. The first non-null value
 * returned from the callback will be returned from this function. If the callback returns null
 * for every line, this function also returns null.
 */
function scanLinesUntilMatch(text, callback) {
    const lines = text.split("\n");
    for (const line of lines) {
        const result = callback(line);
        if (result !== null) {
            return result;
        }
    }
    return null;
}

/**
 * Similar to scanLinesUntilMatch, but scans through the contents of a file.
 */
function scanFileUntilMatch(file, callback) {
    const text = cat(file);
    return scanLinesUntilMatch(text, callback);
}

function waitForExitCode(logpath) {
    const kAttempts = 300;
    const kRetryDelayMs = 500;

    for (let i = 0; i < kAttempts; i++) {
        const isFinalAttempt = (i == kAttempts - 1);
        // We ignore parse errors on all but our last attempt to read the file. This is because
        // we're reading the file as it's being written and we could read a partial line at some
        // point. When we're about to give up though, we want a parse error to be fatal.
        const exitCode = scanFileUntilMatch(logpath, isFinalAttempt ? getExitCode : tryGetExitCode);
        if (exitCode !== null) {
            return exitCode;
        }
        sleep(kRetryDelayMs);
    }

    return null;
}

function getPidFromRawProgramOutput(rawOutput) {
    return scanLinesUntilMatch(rawOutput, line => {
        if (line.includes("forked process: ")) {
            const arr = line.split(": ");
            assert.eq(2, arr.length, "Failed to parse forked process pid");
            const pid = parseInt(arr[1]);
            assert(!Number.isNaN(pid), "Failed to parse forked process pid");
            return pid;
        }
        return null;
    });
}

function forkThenShutdownMongoProgram(program, args) {
    // Launch the parent process and check it exits with code 0.
    const logpath = args.logpath;
    resetDbpath(args.dbpath);
    const arrOpts = MongoRunner.arrOptions(program, args);
    jsTestLog("Launching [" + arrOpts + "]");

    clearRawMongoProgramOutput();
    assert.eq(0, runMongoProgram.apply(this, arrOpts));
    const rawOutput = rawMongoProgramOutput(".*");
    const pid = getPidFromRawProgramOutput(rawOutput);
    assert.neq(null, pid, "Failed to parse forked process pid");

    // Check the logs to see that the child process exits with code 0 as well.
    jsTestLog("Waiting for forked process to shut down, pid=" + pid);
    const exitCode = waitForExitCode(logpath, pid);
    if (exitCode === null) {
        jsTestLog("Killing process with pid " + pid + " due to not observing shutdown.");
        assert.eq(
            0, runNonMongoProgram("kill", "-s", "SIGKILL", pid), "Failed to kill forked process");
        assert(false, "Didn't observe shutdown message in forked process' log file");
    }
    jsTestLog("Forked process exited with code " + exitCode);
    assert.eq(0, exitCode);
}

/**
 * Starts mongod with the given arguments plus --fork, sets a failpoint that will cause the forked
 * process to shutdown immediately after startup completes, and asserts that both the parent and
 * child process exit with a code of 0.
 */
export function forkThenShutdownMongod(args = {}) {
    const defaultArgs = {
        fork: '',
        setParameter: {
            'failpoint.shutdownAtStartup': '{mode:"alwaysOn"}',
        },
        useLogFiles: true,
        waitForConnect: false,
    };

    const testArgs = MongoRunner.mongodOptions({...defaultArgs, ...args});
    forkThenShutdownMongoProgram('mongod', testArgs);
}

/**
 * Starts mongos with the given config server and arguments (plus --fork) sets a failpoint that
 * will cause the forked process to shutdown immediately after startup completes, and asserts that
 * both the parent and child process exit with a code of 0.
 */
export function forkThenShutdownMongos(configRS, args = {}) {
    const defaultArgs = {
        fork: '',
        setParameter: {
            'failpoint.shutdownAtStartup': '{mode:"alwaysOn"}',
        },
        configdb: configRS.getURL(),
        useLogFiles: true,
        waitForConnect: false,
    };

    const testArgs = MongoRunner.mongosOptions({...defaultArgs, ...args});
    forkThenShutdownMongoProgram('mongos', testArgs);
}
