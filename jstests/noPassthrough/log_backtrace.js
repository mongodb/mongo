/**
 * Validate that backtraces are logged to a separate file
 *
 * @tags: [requires_persistence]
 */

(function() {
'use strict';

function waitFailedToStart(pid, exitCode) {
    assert.soon(function() {
        return !checkProgram(pid).alive;
    }, `Failed to wait for ${pid} to die`, 3 * 60 * 1000);

    assert.eq(exitCode,
              checkProgram(pid).exitCode,
              `Failed to wait for ${pid} to die with exit code ${exitCode}`);
}

function parseLogFile(file) {
    const result = cat(file);
    const json_str = result.split("\n")[0];
    try {
        return JSON.parse(json_str);
    } catch (e) {
        jsTestLog("Failed to parse: " + result + "\n" + json_str);
        throw e;
    }
}

// Run MongoD successfully so we populate the data directory
var m = MongoRunner.runMongod({});

assert.commandWorked(m.getDB("test").foo.insert({x: 1}));

MongoRunner.stopMongod(m);

// Make sure it is stopped before we manipulate the turtle file
waitFailedToStart(m.pid, 0);

// Sleep
sleep(5000);

const dbpath = m.dbpath.replace("\\", "/");
const backtraceLogFile = `backtrace.log`;
const backtraceLogFileFullPath = dbpath + "/" + backtraceLogFile;

print(dbpath);

// Corrupt the WiredTiger. turtle file to trigger a backtrace
const command = `echo xxxxxxxxxxxxxxxxxxxxx > ${dbpath}/WiredTiger.turtle`;

let ret;
if (_isWindows()) {
    ret = runProgram('cmd.exe', '/c', command);
} else {
    ret = runProgram('/bin/sh', '-c', command);
}

assert.eq(ret, 0);

// Restart MongoD with the corrupted turtle file
m = MongoRunner.runMongod({
    setParameter: "backtraceLogFile=" + backtraceLogFileFullPath,
    dbpath: dbpath,
    restart: true,
    cleanData: false,
    waitForConnect: false
});

if (_isWindows()) {
    waitFailedToStart(m.pid, 14);  // MongoRunner.EXIT_ABORT
} else {
    waitFailedToStart(m.pid, 6);  // MongoRunner.EXIT_ABORT
}

// Check we have one log line
let log = parseLogFile(backtraceLogFileFullPath);
assert.eq(log["id"], 31380);
assert.eq(log["msg"], "BACKTRACE");
})();
