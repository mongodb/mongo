/**
 * The dropIndexes command has to have the same assertions on the primary and secondary nodes.
 *
 * dropIndexes for applyOps will return 'BackgroundOperationInProgressForNamespace' if there are any
 * in-progress index builds. For initial sync, this causes all of the in-progress index builds to be
 * aborted. However, during steady state replication, the dropIndexes for applyOps would hang until
 * there are no more in-progress index builds. But because the abortIndexBuild/commitIndexBuild
 * oplog entries come after the dropIndexes oplog entry, replication will stall indefinitely waiting
 * for this condition.
 *
 * This happens because on the primary, the dropIndexes command would abort in-progress index builds
 * and drop any ready indexes even if there are index builds in-progress. To solve this problem, the
 * dropIndexes command cannot drop any ready indexes while there are any in-progress index builds.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const replSet = new ReplSetTest({
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

replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const dbName = "test";
const collName = "drop_indexes_prevents_dropping_ready_indexes_after_aborting";

const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();
const db = primary.getDB(dbName);
const coll = db.getCollection(collName);

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({a: i, b: i}));
}

jsTestLog("Starting an index build on {a: 1} and hanging on the primary");
IndexBuildTest.pauseIndexBuilds(primary);
let awaitIndexBuild = IndexBuildTest.startIndexBuild(
    primary, coll.getFullName(), {a: 1}, {}, [ErrorCodes.IndexBuildAborted]);
IndexBuildTest.waitForIndexBuildToStart(primary.getDB(dbName), coll.getName(), "a_1");

const failPoint = "hangAfterAbortingIndexes";
let res = assert.commandWorked(db.adminCommand({configureFailPoint: failPoint, mode: "alwaysOn"}));
let timesEntered = res.count;

TestData.dbName = dbName;
TestData.collName = collName;

jsTestLog(
    "Aborting index build on {a: 1} and hanging dropIndexes while yielding the collection lock");
let awaitDropIndexes = startParallelShell(() => {
    assert.commandFailedWithCode(db.getSiblingDB(TestData.dbName)
                                     .runCommand({dropIndexes: TestData.collName, index: ["a_1"]}),
                                 ErrorCodes.BackgroundOperationInProgressForNamespace);
}, primary.port);

awaitIndexBuild();
assert.commandWorked(primary.adminCommand({
    waitForFailPoint: failPoint,
    timesEntered: timesEntered + 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Creating the index {a: 1} to completion");
IndexBuildTest.resumeIndexBuilds(primary);
assert.commandWorked(coll.createIndex({a: 1}));

jsTestLog("Starting an index build on {b: 1} and hanging on the secondary");
IndexBuildTest.pauseIndexBuilds(secondary);
awaitIndexBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {b: 1}, {}, [], 2);
IndexBuildTest.waitForIndexBuildToStart(secondary.getDB(dbName), coll.getName(), "b_1");

jsTestLog("Resuming the dropIndexes command");
assert.commandWorked(db.adminCommand({configureFailPoint: failPoint, mode: "off"}));
awaitDropIndexes();

jsTestLog("Waiting for dropIndexes to replicate");
replSet.awaitReplication();

jsTestLog("Resuming the index build {b: 1} on the secondary");
IndexBuildTest.resumeIndexBuilds(secondary);
awaitIndexBuild();

replSet.stopSet();
}());
