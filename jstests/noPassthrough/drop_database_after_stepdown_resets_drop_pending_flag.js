/**
 * Verifies that a dropDatabase operation interrupted due to stepping down resets the drop pending
 * flag. Additionally, after the node steps down, we ensure it can drop the database as instructed
 * by the new primary.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {},
    ]
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

if (!IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    jsTestLog('Two phase index builds not supported, skipping test.');
    rst.stopSet();
    return;
}

const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(testDB.adminCommand(
    {configureFailPoint: 'dropDatabaseHangAfterWaitingForIndexBuilds', mode: 'alwaysOn'}));

assert.commandWorked(coll.insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(primary);

let awaitIndexBuild = startParallelShell(() => {
    const coll = db.getSiblingDB('test').getCollection('test');
    assert.commandFailedWithCode(coll.createIndex({a: 1}), ErrorCodes.IndexBuildAborted);
}, primary.port);

IndexBuildTest.waitForIndexBuildToStart(testDB, coll.getName(), "a_1");

let awaitDropDatabase = startParallelShell(() => {
    assert.commandFailedWithCode(db.getSiblingDB('test').dropDatabase(),
                                 ErrorCodes.InterruptedDueToReplStateChange);
}, primary.port);

checkLog.containsJson(primary, 4612300);

assert.commandWorked(testDB.adminCommand({clearLog: "global"}));
let awaitStepDown = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({replSetStepDown: 30}));
}, primary.port);

IndexBuildTest.resumeIndexBuilds(primary);

checkLog.containsJson(primary, 21344);
assert.commandWorked(testDB.adminCommand(
    {configureFailPoint: 'dropDatabaseHangAfterWaitingForIndexBuilds', mode: 'off'}));

awaitIndexBuild();
awaitDropDatabase();
awaitStepDown();

rst.awaitReplication();

// Have the new primary try to drop the database. The stepped down node must successfully replicate
// this dropDatabase command.
assert.commandWorked(rst.getPrimary().getDB('test').dropDatabase());
rst.stopSet();
})();
