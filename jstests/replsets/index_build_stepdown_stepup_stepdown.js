/*
 * This test starts an index build, then through a series of failovers, demonstrates that the index
 * build does not miss any writes from outstanding prepared transactions.  This is an issue since
 * prepared transactions do not use locks on secondaries, and thus can allow an index build to
 * commit without waiting for the prepared transaction.
 *
 * @tags: [
 *   requires_document_locking,
 *   requires_replication,
 *   uses_prepare_transaction,
 *   uses_transactions,
 *   ]
 */
load("jstests/replsets/rslib.js");
load("jstests/core/txns/libs/prepare_helpers.js");

(function() {

"use strict";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const dbName = jsTestName();
const collName = "coll";

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const collNss = primaryColl.getFullName();

jsTestLog("Do document writes");
for (var i = 0; i < 4; i++) {
    assert.commandWorked(primaryColl.insert({_id: i, y: i}, {"writeConcern": {"w": 1}}));
}
rst.awaitReplication();

const secondary = rst.getSecondary();
const secondaryAdmin = secondary.getDB("admin");
const secondaryColl = secondary.getDB(dbName)[collName];

assert.commandWorked(
    secondary.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 2}));

jsTestLog("Enable setYieldAllLocksHang fail point");
assert.commandWorked(secondaryAdmin.runCommand(
    {configureFailPoint: "setYieldAllLocksHang", data: {namespace: collNss}, mode: "alwaysOn"}));

TestData.dbName = dbName;
TestData.collName = collName;

const indexThread = startParallelShell(() => {
    jsTestLog("Create index");
    const primaryDB = db.getSiblingDB(TestData.dbName);
    assert.commandWorked(primaryDB[TestData.collName].createIndex({"y": 1}));
}, primary.port);

// Wait until index build (collection scan phase) on secondary yields.
jsTestLog("Wait for the hybrid index build on secondary to hang");
assert.soon(
    function() {
        const result = secondaryAdmin.currentOp({"command.createIndexes": collName});
        assert.commandWorked(result);
        if (result.inprog.length === 1 && result.inprog[0].numYields > 0) {
            return true;
        }

        return false;
    },
    function() {
        return "Failed to find operation in currentOp() output: " +
            tojson(secondaryAdmin.currentOp());
    },
    30 * 1000);

jsTestLog("Step up the current secondary");
assert.commandWorked(secondary.adminCommand({"replSetStepUp": 1}));
waitForState(secondary, ReplSetTest.State.PRIMARY);
waitForState(primary, ReplSetTest.State.SECONDARY);
const newPrimary = rst.getPrimary();

// Make sure the secondary was able to step up successfully.
assert.eq(newPrimary, secondary);

jsTestLog("Start a txn");
const session = newPrimary.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 20}));

jsTestLog("Prepare txn");
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

// Make the hybrid index build hang after first drain
assert.commandWorked(newPrimary.adminCommand(
    {configureFailPoint: "hangAfterIndexBuildFirstDrain", mode: "alwaysOn"}));

// This will make the hybrid build previously started on secondary (now primary) resume.
jsTestLog("Disable setYieldAllLocksHang fail point");
assert.commandWorked(
    newPrimary.adminCommand({configureFailPoint: "setYieldAllLocksHang", mode: "off"}));

// Wait until the first drain completed.
checkLog.contains(newPrimary, "Hanging after index build first drain");

assert.commandWorked(
    newPrimary.adminCommand({configureFailPoint: "hangAfterIndexBuildFirstDrain", mode: "off"}));

jsTestLog("Step up the old primary");
assert.commandWorked(primary.adminCommand({"replSetStepUp": 1}));
waitForState(primary, ReplSetTest.State.PRIMARY);
rst.awaitReplication();

// Create a proxy session in order to complete the prepared transaction.
const newSession = new _DelegatingDriverSession(primary, session);

assert.commandWorked(PrepareHelpers.commitTransaction(newSession, prepareTimestamp));
indexThread();
rst.awaitReplication();

assert.soon(() => 2 == primaryColl.getIndexes().length);
assert.soon(() => 2 == secondaryColl.getIndexes().length);

rst.stopSet();
})();
