/**
 * Confirms that aborting a background index build on a secondary during step up does not leave the
 * node in an inconsistent state.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            slowms: 30000,  // Don't log slow operations on secondary. See SERVER-44821.
        },
    ]
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

let secondary = rst.getSecondary();
IndexBuildTest.pauseIndexBuilds(secondary);

const awaitIndexBuild =
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {background: true}, [
        ErrorCodes.InterruptedDueToReplStateChange
    ]);

// When the index build starts, find its op id.
let secondaryDB = secondary.getDB(testDB.getName());
const opId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

IndexBuildTest.assertIndexBuildCurrentOpContents(secondaryDB, opId, (op) => {
    jsTestLog('Inspecting db.currentOp() entry for index build: ' + tojson(op));
    assert.eq(coll.getFullName(),
              op.ns,
              'Unexpected ns field value in db.currentOp() result for index build: ' + tojson(op));
});

// Step up the secondary and hang the process.
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "hangIndexBuildOnStepUp", mode: "alwaysOn"}));
const awaitStepUp = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({replSetStepUp: 1}));
}, secondary.port);

awaitIndexBuild();

assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: "hangIndexBuildOnStepUp",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Kill the index build on the secondary as it is stepping up.
assert.commandWorked(secondaryDB.killOp(opId));

// Finish the step up.
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "hangIndexBuildOnStepUp", mode: "off"}));
rst.waitForState(secondary, ReplSetTest.State.PRIMARY);
awaitStepUp();

rst.awaitReplication();

IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

const secondaryColl = secondaryDB.getCollection(coll.getName());
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);

rst.stopSet();
})();
