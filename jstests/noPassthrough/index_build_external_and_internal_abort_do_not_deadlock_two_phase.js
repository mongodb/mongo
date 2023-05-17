/**
 * Tests dropping a collection (causing an external index build abort) does not deadlock with an
 * internal self abort for two-phase index builds.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
    ]
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB('test');
const primaryColl = primaryDB.getCollection('test');

primaryColl.drop();
assert.commandWorked(primaryColl.insert({a: 1}));

// Pause the index builds on the secondary, using the 'hangAfterStartingIndexBuild' failpoint.
const hangBeforeCleanup = configureFailPoint(primaryDB, 'hangIndexBuildBeforeAbortCleanUp');
const hangAfterIndexBuildSecondDrain =
    configureFailPoint(primaryDB, "hangAfterIndexBuildSecondDrain");

// Block secondary to avoid commitQuorum being fullfilled.
IndexBuildTest.pauseIndexBuilds(rst.getSecondary());

jsTestLog("Waiting for index build to start");
const createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {a: 1}, null, [ErrorCodes.Interrupted]);

// Make sure we get the builder's opId, instead of the connection's.
const filter = {
    "desc": {$regex: /IndexBuildsCoordinatorMongod.*/}
};
const opId =
    IndexBuildTest.waitForIndexBuildToStart(primaryDB, primaryColl.getName(), 'a_1', filter);
const buildUUID =
    IndexBuildTest
        .assertIndexesSoon(primaryColl, 2, ['_id_'], ['a_1'], {includeBuildUUIDs: true})['a_1']
        .buildUUID;

hangAfterIndexBuildSecondDrain.wait();
jsTestLog("Running killOp to force self-abort on primary");
assert.commandWorked(primaryDB.killOp(opId));
hangAfterIndexBuildSecondDrain.off();

// Wait for the index build to be in clean up path.
hangBeforeCleanup.wait();

const hangAfterCollDropHasLocks =
    configureFailPoint(primaryDB, 'hangAbortIndexBuildByBuildUUIDAfterLocks');

const collDrop = startParallelShell(funWithArgs(function(dbName, collName) {
                                        jsTestLog("Dropping collection");
                                        db.getSiblingDB(dbName).getCollection(collName).drop();
                                    }, primaryDB.getName(), primaryColl.getName()), primary.port);

hangAfterCollDropHasLocks.wait();
hangBeforeCleanup.off();
hangAfterCollDropHasLocks.off();

jsTestLog("Waiting for the index build to be killed");
// "Index build: joined after abort".
checkLog.containsJson(primary, 20655, {
    buildUUID: function(uuid) {
        return uuid && uuid["uuid"]["$uuid"] === extractUUIDFromObject(buildUUID);
    }
});

jsTestLog("Waiting for collection drop shell to return");
collDrop();
createIdx();

rst.stopSet();
})();
