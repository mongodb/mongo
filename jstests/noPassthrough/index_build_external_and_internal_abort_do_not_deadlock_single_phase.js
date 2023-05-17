/**
 * Tests dropping a collection (causing an external index build abort) does not deadlock with an
 * internal self abort for single-phase index builds.
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load("jstests/libs/fail_point_util.js");

// A standalone configuration is key to running the index build single-phase.
const conn = MongoRunner.runMongod();

const dbName = 'test';
const collName = 'coll';
const db = conn.getDB(dbName);
const coll = db.getCollection(collName);

coll.drop();
assert.commandWorked(coll.insert({a: 1}));

// Hang after the index build has checked if the build is already aborted, but before taking
// collection locks for cleanup.
const hangBeforeCleanup = configureFailPoint(db, 'hangIndexBuildBeforeAbortCleanUp');
const hangAfterIndexBuildDumpsInsertsFromBulk =
    configureFailPoint(db, "hangAfterIndexBuildDumpsInsertsFromBulk");

const createIdx = IndexBuildTest.startIndexBuild(
    conn, coll.getFullName(), {a: 1}, null, [ErrorCodes.Interrupted]);
// Make sure we get the builder's opId, instead of the connection's.
const filter = {
    "desc": {$regex: /IndexBuildsCoordinatorMongod.*/}
};
const opId = IndexBuildTest.waitForIndexBuildToStart(db, collName, 'a_1', filter);

hangAfterIndexBuildDumpsInsertsFromBulk.wait();
jsTestLog("Running killOp to force self-abort");
assert.commandWorked(db.killOp(opId));
hangAfterIndexBuildDumpsInsertsFromBulk.off();

hangBeforeCleanup.wait();

const hangAfterCollDropHasLocks =
    configureFailPoint(db, 'hangAbortIndexBuildByBuildUUIDAfterLocks');
const collDrop = startParallelShell(funWithArgs(function(dbName, collName) {
                                        db.getSiblingDB(dbName).getCollection(collName).drop();
                                    }, dbName, collName), conn.port);

hangAfterCollDropHasLocks.wait();
hangBeforeCleanup.off();
hangAfterCollDropHasLocks.off();

jsTestLog("Waiting for collection drop shell to return");
collDrop();
createIdx();

MongoRunner.stopMongod(conn);
})();
