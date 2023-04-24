/**
 * Sends a killop to an index build on a secondary node before it commits and confirms that the
 * index build is canceled on all nodes.
 *
 * @tags: [
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");
load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary, but allow it to participate in commitQuorum.
            rsConfig: {
                priority: 0,
            },
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

// Pause the index build on the primary so that it does not commit.
IndexBuildTest.pauseIndexBuilds(primary);

const secondary = rst.getSecondary();
IndexBuildTest.pauseIndexBuilds(secondary);

const createIdx = IndexBuildTest.startIndexBuild(
    primary, coll.getFullName(), {a: 1}, {}, ErrorCodes.IndexBuildAborted);

// When the index build starts, find its op id.
const secondaryDB = secondary.getDB(testDB.getName());
const opId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

IndexBuildTest.assertIndexBuildCurrentOpContents(secondaryDB, opId, (op) => {
    jsTestLog('Inspecting db.currentOp() entry for index build: ' + tojson(op));
    assert.eq(coll.getFullName(),
              op.ns,
              'Unexpected ns field value in db.currentOp() result for index build: ' + tojson(op));
});

// Kill the index build.
assert.commandWorked(secondaryDB.killOp(opId));

// Resume index build, allowing it to cancel.
IndexBuildTest.resumeIndexBuilds(secondary);
// "attempting to abort index build".
checkLog.containsJson(primary, 4656010);

IndexBuildTest.resumeIndexBuilds(primary);
// "Index build: joined after abort".
checkLog.containsJson(primary, 20655);

// Wait for the index build abort to replicate.
rst.awaitReplication();

// Expect the index build to fail and for the index to not exist on either node.
createIdx();

IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

const secondaryColl = secondaryDB.getCollection(coll.getName());
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);

rst.stopSet();
})();
