/**
 * Confirms that aborting a background index builds on a secondary does not leave node in an
 * inconsistent state.
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
            slowms: 30000,  // Don't log slow operations on secondary. See SERVER-44821.
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

let secondary = rst.getSecondary();
IndexBuildTest.pauseIndexBuilds(secondary);

const createIdx =
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {background: true});

// When the index build starts, find its op id.
let secondaryDB = secondary.getDB(testDB.getName());
const opId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

IndexBuildTest.assertIndexBuildCurrentOpContents(secondaryDB, opId, (op) => {
    jsTestLog('Inspecting db.currentOp() entry for index build: ' + tojson(op));
    assert.eq(coll.getFullName(),
              op.ns,
              'Unexpected ns field value in db.currentOp() result for index build: ' + tojson(op));
});

// Kill the index build. This should crash the secondary.
assert.commandWorked(secondaryDB.killOp(opId));

assert.soon(function() {
    return rawMongoProgramOutput().search(/Fatal assertion.*(51101|31354)/) >= 0;
});

// After restarting the secondary, expect that the index build completes successfully.
const fassertProcessExitCode = _isWindows() ? MongoRunner.EXIT_ABRUPT : MongoRunner.EXIT_ABORT;
rst.stop(secondary.nodeId, undefined, {forRestart: true, allowedExitCode: fassertProcessExitCode});
rst.start(secondary.nodeId, undefined, true /* restart */);

secondary = rst.getSecondary();
secondaryDB = secondary.getDB(testDB.getName());

// Wait for the index build to complete.
rst.awaitReplication();

// Expect successful createIndex command invocation in parallel shell. A new index should be present
// on the primary and secondary.
createIdx();
IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);

// Check that index was created on the secondary despite the attempted killOp().
const secondaryColl = secondaryDB.getCollection(coll.getName());
IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 'a_1']);

rst.stopSet();
})();
