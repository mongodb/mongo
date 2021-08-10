/**
 * Test that only the primary can reap a session and remove the config.transactions doc for the
 * session, and that arbiters never try to a reap session.
 */

(function() {
"use strict";

// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

let replTest = new ReplSetTest({
    name: 'reaping',
    nodes: [
        {/* primary */},
        {/* secondary */ rsConfig: {priority: 0}},
        {/* arbiter */ rsConfig: {arbiterOnly: true}}
    ],
    nodeOptions: {
        setParameter: {
            TransactionRecordMinimumLifetimeMinutes: 0,
            featureFlagRetryableFindAndModify: true,
            storeFindAndModifyImagesInSideCollection: true
        }
    }
});
let nodes = replTest.startSet();

replTest.initiate();
let primary = replTest.getPrimary();
let sessionsCollOnPrimary = primary.getDB("config").system.sessions;
let transactionsCollOnPrimary = primary.getDB("config").transactions;
let imageCollOnPrimary = primary.getDB("config").image_collection;

replTest.awaitSecondaryNodes();
let secondary = replTest.getSecondary();
let arbiter = replTest.getArbiter();

const dbName = jsTestName();
const collName = "test";
const reapErrorMsgRegex =
    new RegExp("Sessions collection is not set up.*waiting until next sessions reap interval");

// Set up a session with a retryable insert and findAndModify.
let session = primary.startSession({retryWrites: 1});
assert.commandWorked(session.getDatabase(dbName)[collName].save({x: 1}));
let result =
    session.getDatabase(dbName)[collName].findAndModify({query: {x: 1}, update: {$set: {y: 1}}});
// The findAndModify helper returns the document and not the whole response. Assert on the value of
// `x`. Though it's expected a command error would become an exception that fails the test.
assert.eq(1, result['x'], "Whole result: " + result);
assert.commandWorked(primary.adminCommand({refreshLogicalSessionCacheNow: 1}));
assert.eq(1, sessionsCollOnPrimary.find().itcount());
assert.eq(1, transactionsCollOnPrimary.find().itcount());
assert.eq(1, imageCollOnPrimary.find().itcount());

// Remove the session doc so the session gets reaped when reapLogicalSessionCacheNow is run.
assert.commandWorked(sessionsCollOnPrimary.remove({}));

// Test that a reap on a secondary does not lead to the on-disk state reaping of the session
// since the session does not exist in the secondary's session catalog.
{
    assert.commandWorked(secondary.adminCommand({clearLog: 'global'}));
    assert.commandWorked(secondary.adminCommand({reapLogicalSessionCacheNow: 1}));

    assert.eq(1, transactionsCollOnPrimary.find().itcount());
    assert.eq(false, checkLog.checkContainsOnce(secondary, reapErrorMsgRegex));
}

// Test that a reap on an arbiter does not lead to reaping of the session.
{
    assert.commandWorked(arbiter.adminCommand({clearLog: 'global'}));
    assert.commandWorked(arbiter.adminCommand({reapLogicalSessionCacheNow: 1}));

    assert.eq(1, transactionsCollOnPrimary.find().itcount());

    if (!jsTest.options().useRandomBinVersionsWithinReplicaSet) {
        // Verify that the arbiter did not try to reap the session.
        assert.eq(false, checkLog.checkContainsOnce(arbiter, reapErrorMsgRegex));
    }
}

// Test that a reap on the primary works as expected.
{
    assert.commandWorked(primary.adminCommand({clearLog: 'global'}));
    assert.commandWorked(primary.adminCommand({reapLogicalSessionCacheNow: 1}));

    assert.eq(0, transactionsCollOnPrimary.find().itcount());
    assert.eq(0, imageCollOnPrimary.find().itcount());
}

replTest.stopSet();
})();
