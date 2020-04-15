/**
 * Test that the shutdown command called on a primary node with {force: true} succeeds even if
 * stepDown fails.
 *
 * 1.  Initiate a 3-node replica set.
 * 2.  Block replication to secondaries.
 * 3.  Write to primary.
 * 4.  Try to shut down primary with {force: true}.
 * 5.  Kill the shutdown command while the shutdown command is waiting to stepDown.
 * 6.  Test that the primary node still shuts down.
 *
 */
(function() {
"use strict";

load("jstests/libs/write_concern_util.js");  // for stopReplicationOnSecondaries.
const replTest = new ReplSetTest({nodes: 3});
replTest.startSet();
replTest.initiateWithHighElectionTimeout();

const primary = replTest.getPrimary();
const testDB = primary.getDB("test");
assert.commandWorked(testDB.foo.insert({x: 1}, {writeConcern: {w: 3}}));

jsTestLog("Blocking replication to secondaries.");
stopReplicationOnSecondaries(replTest);

jsTestLog("Executing write to primary.");
assert.commandWorked(testDB.foo.insert({x: 2}));

jsTestLog("Shutting down primary in a parallel shell");
const shutdownShell = startParallelShell(function() {
    db.adminCommand({shutdown: 1, timeoutSecs: 60, force: true});
}, primary.port);

let shutdownOpID = -1;
let res = {};
jsTestLog("Looking for shutdown in currentOp() output");
assert.soon(function() {
    res = primary.getDB('admin').currentOp(true);
    for (const index in res.inprog) {
        const entry = res.inprog[index];
        if (entry["command"] && entry["command"]["shutdown"] === 1) {
            shutdownOpID = entry.opid;
            return true;
        }
    }
    return false;
}, "No shutdown command found: " + tojson(res));

jsTestLog("Killing shutdown command on primary.");
primary.getDB('admin').killOp(shutdownOpID);

jsTestLog("Verifying primary shut down and cannot be connected to.");
const exitCode = shutdownShell({checkExitSuccess: false});
assert.neq(0, exitCode, "expected shutdown to close the shell's connection");
assert.soonNoExcept(function() {
    // The parallel shell exits while shutdown is in progress, and if this happens early enough,
    // the primary can still accept connections despite successfully starting to shutdown.
    // So, retry connecting until connections cannot be established and an error is thrown.
    assert.throws(function() {
        new Mongo(primary.host);
    });
    return true;
}, "expected primary node to shut down and not be connectable");

replTest.stopSet();
})();
