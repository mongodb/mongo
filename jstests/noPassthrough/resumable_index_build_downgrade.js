/**
 * Tests that no resumable index build resources are written to disk when a downgrade is in
 * progress.
 *
 * @tags: [requires_persistence, requires_replication]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");

const collName = "resumable_index_build_downgrade";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const coll = testDB.getCollection(collName);

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

const fp = configureFailPoint(primary, "leaveIndexBuildUnfinishedForShutdown");

const createIndex = function(collName) {
    assert.commandFailed(db.getCollection(collName).createIndex({a: 1}));
};
const awaitCreateIndex = startParallelShell(funWithArgs(createIndex, collName), primary.port);

fp.wait();

// We wait for log message 20449, which signals that the index build did not complete. We then
// check that log message 4841502, which would signal that the resumable index build state was
// written to disk and would necessarily come before log message 20449, did not appear.
checkLog.containsJson(primary, 20449, {
    error: function(error) {
        return error.code === ErrorCodes.InterruptedAtShutdown;
    }
});
assert(!checkLog.checkContainsOnceJson(primary, 4841502));

awaitCreateIndex();
rst.stopSet();
})();