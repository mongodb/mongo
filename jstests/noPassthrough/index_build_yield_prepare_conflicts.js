/*
 * This test ensures it is not possible for the collection scan phase of an index build to encounter
 * a prepare conflict after yielding. See SERVER-44577.
 *
 * @tags: [
 *   requires_replication,
 *   uses_transactions,
 *   uses_prepare_transaction,
 * ]
 */
load("jstests/core/txns/libs/prepare_helpers.js");  // For PrepareHelpers.
load("jstests/noPassthrough/libs/index_build.js");  // For IndexBuildTest
load("jstests/libs/fail_point_util.js");

(function() {

"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = jsTestName();
const collName = "coll";

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryAdmin = primary.getDB('admin');
const primaryColl = primaryDB[collName];
const collNss = primaryColl.getFullName();

for (var i = 0; i < 3; i++) {
    assert.commandWorked(primaryColl.insert({_id: i, x: i}));
}
rst.awaitReplication();

// Make the index build collection scan yield often.
assert.commandWorked(primary.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 2}));

jsTestLog("Enable setYieldAllLocksHang fail point");
let res = assert.commandWorked(primaryAdmin.runCommand(
    {configureFailPoint: "setYieldAllLocksHang", data: {namespace: collNss}, mode: "alwaysOn"}));
let timesEntered = res.count;

jsTestLog("Create index");
const awaitIndex = IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {x: 1});

// Wait until index build (collection scan phase) yields.
jsTestLog("Wait for the index build to yield and hang");
assert.commandWorked(primaryAdmin.runCommand({
    waitForFailPoint: "setYieldAllLocksHang",
    timesEntered: timesEntered + 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

jsTestLog("Start a txn");
const session = primary.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 20}));

jsTestLog("Prepare txn");
PrepareHelpers.prepareTransaction(session);

// This will make the hybrid build previously started resume.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "setYieldAllLocksHang", mode: "off"}));

jsTestLog("Wait for index build to complete collection scanning phase");
checkLog.containsJson(primary, 20391);

session.abortTransaction_forTesting();
awaitIndex();

rst.stopSet();
})();
